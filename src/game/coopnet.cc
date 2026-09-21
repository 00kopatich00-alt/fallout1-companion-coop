#include "game/coopnet.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

#include <SDL.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
typedef SOCKET CoopSocket;
#define COOP_INVALID_SOCKET INVALID_SOCKET
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int CoopSocket;
#define COOP_INVALID_SOCKET (-1)
#endif

#include "game/actions.h"
#include "game/anim.h"
#include "game/combat.h"
#include "game/combatai.h"
#include "game/critter.h"
#include "game/display.h"
#include "game/game.h"
#include "game/item.h"
#include "game/main.h"
#include "game/map.h"
#include "game/object.h"
#include "game/party.h"
#include "game/protinst.h"
#include "game/scripts.h"
#include "game/stat.h"
#include "game/tile.h"
#include "game/worldmap.h"
#include "plib/color/color.h"
#include "plib/gnw/debug.h"
#include "plib/gnw/gnw.h"
#include "plib/gnw/input.h"
#include "plib/gnw/svga.h"
#include "plib/gnw/intrface.h"
#include "plib/gnw/rect.h"
#include "plib/gnw/text.h"

namespace fallout {

// ---------------------------------------------------------------------------
// Wire protocol (milestone 1: LAN connect + watch each other move)
// ---------------------------------------------------------------------------

// Sized to comfortably fit the largest message on the wire (currently
// CoopDialogueState, ~1.3KB) -- every coopnet_try_recv_message() call site
// stack-allocates a buffer of this size to receive into.
const int kCoopMaxMessagePayload = 2048;

#pragma pack(push, 1)

struct CoopMsgHeader {
    uint16_t length; // length of payload that follows, not including this header
    uint8_t type;
};

enum CoopMsgType : uint8_t {
    COOP_MSG_HELLO = 1, // client -> host, first message after connect
    COOP_MSG_HELLO_ACK = 2, // host -> client, accept/reject + companion identity
    COOP_MSG_MOVE_INTENT = 3, // client -> host, "walk companion to this tile"
    COOP_MSG_POSITION = 4, // host -> client, one object's authoritative position
    COOP_MSG_HEARTBEAT = 5, // either direction, keepalive
    COOP_MSG_ITEM_DROPPED = 6, // either direction, an item appeared on the ground
    COOP_MSG_ITEM_PICKED_UP = 7, // either direction, a ground item was picked up
    COOP_MSG_PICKUP_REQUEST = 8, // client -> host, "have the companion pick up this ground item"
    COOP_MSG_USE_REQUEST = 9, // client -> host, "have the companion use this scenery object (e.g. a door)"
    COOP_MSG_COMPANION_INVENTORY = 10, // host -> client, full snapshot of the companion's current inventory
    COOP_MSG_COMBAT_TURN = 11, // host -> client, companion's combat turn started/ended + current AP
    COOP_MSG_COMBAT_ACTION = 12, // client -> host, the companion's chosen combat action (move or end-turn for now)
    COOP_MSG_COMBAT_BEGIN = 13, // host -> client, a synced combat has started
    COOP_MSG_COMBAT_END = 14, // host -> client, the synced combat has ended
    COOP_MSG_COMBAT_PARTICIPANT = 15, // host -> client, one combat participant's identity + current state
    COOP_MSG_MAP_TRANSITION = 16, // host -> client, the host's local map just changed
    COOP_MSG_DIALOGUE_BEGIN = 17, // host -> client, the host started a conversation
    COOP_MSG_DIALOGUE_STATE = 18, // host -> client, the currently-displayed NPC line + option texts
    COOP_MSG_DIALOGUE_END = 19, // host -> client, the conversation ended
    COOP_MSG_GAME_OVER = 20, // host -> client, the shared game has ended
    COOP_MSG_SKILL_REQUEST = 21, // client -> host, "have the companion use this skill on this target"
};

const uint32_t kCoopProtocolVersion = 1;

struct CoopHello {
    uint32_t protocolVersion;
    char mapName[16];
};

struct CoopHelloAck {
    uint8_t accepted;
    int32_t companionPid;
    int32_t companionTile;
    int32_t companionElevation;
};

struct CoopMoveIntent {
    int32_t targetTile;
};

struct CoopPosition {
    uint8_t which; // 0 = companion, 1 = obj_dude
    int32_t tile;
    int32_t elevation;
    int32_t rotation;
};

// actionPoints > 0 means it's now the companion's combat turn (with this
// many AP to spend); actionPoints <= 0 means the turn just ended (client
// should stop sending COOP_MSG_COMBAT_ACTION until the next one of these).
struct CoopCombatTurn {
    int32_t actionPoints;
};

enum CoopCombatActionType : uint8_t {
    COOP_COMBAT_ACTION_MOVE = 0,
    COOP_COMBAT_ACTION_END_TURN = 1,
    // If targetId is a valid, currently-known participant id, attacks that
    // specific participant (combat_attack() against the real host-side
    // object it maps to). Otherwise (targetId == -1, or an id the host
    // doesn't recognize -- e.g. it died before the click round-tripped)
    // falls back to combat_ai()'s own auto-targeting, same as before
    // participant mirroring existed.
    COOP_COMBAT_ACTION_ATTACK = 2,
};

struct CoopCombatAction {
    uint8_t actionType;
    int32_t targetTile; // valid when actionType == COOP_COMBAT_ACTION_MOVE
    int32_t targetId; // valid when actionType == COOP_COMBAT_ACTION_ATTACK; -1 = auto-target
};

// Identifies a combat participant (an enemy, or an ally other than obj_dude/
// the companion, which already have their own dedicated sync) by the host's
// own Object::id -- a purely local, opaque token the client never needs to
// interpret, just remember alongside whichever local object it associates
// with that id (see g_coopParticipants) and echo back when the player
// targets that enemy. Sent host -> client once per participant at combat
// start and periodically (piggybacking on the same broadcast cadence as
// COOP_MSG_POSITION) while that participant is still alive.
struct CoopCombatParticipant {
    int32_t id;
    int32_t pid;
    int32_t tile;
    int32_t elevation;
    int32_t rotation;
    int32_t hp;
    uint8_t isDead;
};

// Used for both COOP_MSG_ITEM_DROPPED and COOP_MSG_ITEM_PICKED_UP. Identifies
// an item by (pid, tile, elevation) rather than a shared unique id — see the
// caveat on coopnet_notify_item_dropped()/coopnet_notify_item_picked_up() in
// coopnet.h.
struct CoopItemEvent {
    int32_t pid;
    int32_t tile;
    int32_t elevation;
};

// Client -> host: "have the companion use this skill (Lockpick, Steal,
// Traps, First Aid, Doctor, Science, Repair) on this target." Same
// (pid, tile, elevation) target-identity caveat as CoopItemEvent -- the
// target can be an item, scenery, or critter depending on the skill, so
// unlike pickup/use requests there's no single expected FID_TYPE to filter
// by when resolving it host-side.
//
// targetIsHostDude is a special case for exactly one real ambiguity: the
// companion deliberately shares obj_dude's own pid (that's what lets it
// reuse the host's real stats/skills for free -- see
// coopnet_on_client_skill_use()'s comment in coopnet.h), so a plain
// (pid, tile, elevation) lookup for "the host's character" could just as
// easily resolve to the companion itself if they're standing near each
// other -- confirmed via testing as the cause of, e.g., Steal silently
// failing (action_use_skill_on() correctly refuses a2 == a1, "can't steal
// from yourself", when the mis-resolved target turned out to be the
// companion). Set when the client's own local target *is* obj_dude (the
// mirrored host character on the client's screen); the host then resolves
// straight to its own obj_dude, skipping the ambiguous search entirely.
struct CoopSkillRequest {
    int32_t skill;
    int32_t pid;
    int32_t tile;
    int32_t elevation;
    uint8_t targetIsHostDude;
};

// The companion's inventory lives as a real, independent Object on each
// side (host's is authoritative; the client's is a mirror puppeted only by
// position broadcasts) -- unlike ground items, its *contents* were never
// synced at all until this, which is why picking something up completed
// correctly on the host (confirmed via testing/debug log) but never showed
// up in the client's own "open inventory" window. Sent host -> client
// whenever the companion's inventory changes (and once on connect), always
// as a full snapshot rather than an incremental diff -- simplest thing that
// can't drift out of sync. Fixed capacity/size (not a true variable-length
// message) to keep wire framing simple; excess items beyond the cap are
// silently dropped from the sync, a known limitation for a very heavily
// loaded companion. Only covers loose inventory contents, not which items
// are equipped in hand/worn slots or partial ammo/charge counts on them.
const int kCoopMaxInventorySyncItems = 30;

struct CoopInventoryItemEntry {
    int32_t pid;
    int32_t quantity;
};

struct CoopInventorySync {
    uint8_t itemCount;
    CoopInventoryItemEntry items[kCoopMaxInventorySyncItems];
};

// Host -> client: the host's own local map just changed (a normal exit/
// elevator was used, or worldmap travel finished loading a new area). The
// client runs its own fully independent copy of the game with its own map
// loaded, so nothing else would ever bring it along -- see
// coopnet_host_check_map_transition()'s comment for the full picture.
struct CoopMapTransition {
    char mapName[16];
    int32_t tile;
    int32_t elevation;
    int32_t rotation;
};

// Host -> client: a read-only mirror of the host's current dialogue screen
// -- the client can never initiate or affect dialogue itself (see
// coopnet_notify_dialogue_state()'s comment and gmouse.cc's Client-role
// guard on the talk click), this is purely so the client can watch what the
// host is doing. Text is copied out of gdialog.cc's own already-resolved
// display buffers (dialogBlock.replyText / dialogBlock.options[i].text --
// real text ready to show, not a message-list id the client would need to
// look up itself) and truncated to fit a small, fixed wire format.
const int kCoopMaxDialogueOptions = 6;
const int kCoopDialogueTextLen = 180;

struct CoopDialogueState {
    char replyText[kCoopDialogueTextLen];
    uint8_t optionCount;
    char optionText[kCoopMaxDialogueOptions][kCoopDialogueTextLen];
};

// Host -> client: the shared game has ended (see coopnet_notify_game_over()'s
// comment in coopnet.h for why the companion dying counts as much as the
// host dying).
struct CoopGameOver {
    uint8_t reason;
};

#pragma pack(pop)

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

enum class CoopConnState {
    Idle,
    Listening, // host: listen socket open, waiting for a client to accept
    WaitingForHello, // host: client accepted, waiting for its HELLO
    Connecting, // client: connect() issued, waiting for it to complete
    WaitingForAck, // client: HELLO sent, waiting for HELLO_ACK
    Connected,
};

static bool g_coopAllowMultipleInstances = false;
static bool g_coopSocketsInitialized = false;
static char g_coopInstanceLabel[64] = "";
static char g_coopConnectTarget[64] = "127.0.0.1";

static CoopRole g_coopRole = CoopRole::None;
static CoopConnState g_coopConnState = CoopConnState::Idle;

static CoopSocket g_coopListenSocket = COOP_INVALID_SOCKET;
static CoopSocket g_coopPeerSocket = COOP_INVALID_SOCKET;

static Object* g_coopCompanion = NULL;

// Client-side only: true while it's the companion's combat turn, per the
// most recent COOP_MSG_COMBAT_TURN received. Gates gmouse.cc's click
// handling between a plain exploration-mode move-intent and a turn-gated
// combat move action, and gates whether an end-turn key press means anything.
static bool g_coopClientCombatTurnActive = false;

// Client-side only: true while a synced combat is happening on the host, per
// the most recent COOP_MSG_COMBAT_BEGIN/_END received. Only used to show a
// one-time notification and avoid repeating it -- see coopnet_apply_combat_begin()/
// coopnet_apply_combat_end().
static bool g_coopClientInCombat = false;

// Client-side only: maps a host-side participant id (an opaque token, see
// CoopCombatParticipant's comment) to whichever local Object mirrors it.
// wasSpawned tracks whether that local object was freshly created (obj_pid_new)
// versus adopted from one that already existed in the client's own
// independently-loaded map -- only spawned ones get cleaned up when combat
// ends, since an adopted one is a real, persistent map object that should be
// left alone.
struct CoopParticipantEntry {
    int32_t hostId;
    Object* localObject;
    bool wasSpawned;

    // Same arrival-based smoothing technique as g_coopLastCommandedTile for
    // the companion/obj_dude (see its comment): only issue a new run once
    // the object has actually reached the last commanded tile, rather than
    // reissuing on every ~100ms broadcast. -1 = nothing commanded yet.
    int lastCommandedTile;
};

const int kCoopMaxParticipants = 32;
static CoopParticipantEntry g_coopParticipants[kCoopMaxParticipants];
static int g_coopParticipantCount = 0;

// Host-side only: true only while coopnet_combat_input()'s own loop is
// running, i.e. genuinely the companion's turn. Gates applying an incoming
// COOP_MSG_COMBAT_ACTION -- without this, a message that arrives late (after
// the turn already moved on) could move the companion out of turn order.
static bool g_coopHostCombatTurnActive = false;

// Host-side only: set by an incoming COOP_MSG_COMBAT_ACTION with actionType
// == COOP_COMBAT_ACTION_END_TURN, read and cleared by coopnet_combat_input().
static bool g_coopHostCombatEndTurnRequested = false;

// The tile we last commanded each synced object to run toward, so repeated
// updates for a still-in-progress run don't keep cancelling and restarting
// the animation. On the client side (coopnet_client_apply_position), a new
// run is only issued once the object has actually arrived at the previously
// commanded tile — position updates arrive every ~100ms, and during
// continuous movement the reported tile is almost always slightly different
// each time (the host keeps walking), so reissuing on every reported change
// restarted the run constantly, which looked like a stop-start "crippled"
// gait even though it wasn't a hard jump (confirmed via testing; a
// fixed-time throttle was tried before this and was still fragile).
// register_object_run_to_tile can smoothly cover several tiles in one call,
// so letting each run reach its actual destination before redirecting is
// both simpler and exact. Host side (coopnet_host_apply_move_intent) reuses
// index 0 for the companion, but redirects immediately on any new click
// (matching vanilla's own interrupt-on-click behavior) rather than waiting
// for arrival. Client side uses index 0/1 for companion/dude per received
// CoopPosition.which. -1 = nothing commanded yet.
static int g_coopLastCommandedTile[2] = { -1, -1 };

static unsigned char g_coopRecvBuffer[4096];
static int g_coopRecvBufferLen = 0;

static uint32_t g_coopLastRecvTimeMs = 0;
static uint32_t g_coopLastBroadcastTimeMs = 0;
static uint32_t g_coopLastFollowCheckTimeMs = 0;
static uint32_t g_coopLastHeartbeatSentTimeMs = 0;

// Host-side only: map_data.name as of the last coopnet_poll_host() tick.
// Compared each tick to detect a completed map transition (see
// coopnet_host_check_map_transition()). Set directly in coopnet_start_host()
// so starting to host mid-map is never mistaken for a transition.
static char g_coopHostLastMapName[16] = "";

// Host-side only: whether COOP_GAME_OVER_COMPANION_DIED has already been
// sent for the current companion, so coopnet_host_check_companion_death()
// only ever fires once (main.cc's game_user_wants_to_quit check keeps
// re-running every remaining tick before the loop actually exits). Reset in
// coopnet_start_host() for the same reason g_coopHostLastMapName is.
static bool g_coopCompanionGameOverSent = false;

// coopnet_send_message()'s retry budget when send() reports the socket
// buffer is full (EWOULDBLOCK) -- confirmed via testing this is a real risk
// during combat specifically: exploration mode's message volume (small,
// infrequent -- a broadcast every 100ms, or one message per discrete click)
// never came close to filling a TCP send buffer, but combat introduces much
// tighter send/receive timing between host and client. Without a bound
// here, a peer that's briefly slow to drain its socket turns this into an
// unbounded busy-spin that never yields back to the OS message pump --
// exactly what an "Application (Not Responding)" hang looks like, and
// confirmed to reproduce this way during combat testing.
const uint32_t kCoopSendTimeoutMs = 2000;

// NOTE: deliberately very generous for now while the engine's background-focus
// behavior (see GNW95_lost_focus) is still being made fully reliable during
// coop sessions. Tighten this back down once that's solid.
const uint32_t kCoopHeartbeatTimeoutMs = 120000;
const uint32_t kCoopHeartbeatIntervalMs = 2000;
const uint32_t kCoopBroadcastIntervalMs = 100;
const uint32_t kCoopFollowCheckIntervalMs = 1500;
const int kCoopFollowDistanceThreshold = 3;

// Briefly disabled during testing over a suspected memory-corruption symptom
// (a garbage-looking name in the examine log) -- turned out to be a red
// herring, that was just the host's own custom character name ("retard").
// The real bug found in the same session, floating stray item sprites, was
// fixed properly (see coopnet_apply_companion_inventory()'s use of
// obj_inven_free()), so this is back on.
const bool kCoopCompanionInventorySyncEnabled = true;

// The companion's animation/action queue (anim.cc's register_begin/_end
// sequences) is not safe to have two in-flight requests on the same object
// at once -- confirmed via testing: sending several COOP_MSG_PICKUP_REQUESTs
// back-to-back (e.g. clicking through a pile of several dropped items) let
// the host receive and dispatch all of them in the same poll tick, each
// calling action_get_an_object() on the companion before the previous one's
// queued sequence had actually run, which silently dropped some of the
// pickups (the client's debug log showed a PICKUP_REQUEST sent with no
// matching ITEM_PICKED_UP ever coming back) even though a "trying to pick
// up" animation was still visibly playing. Host-side requests -- pickup and
// (later) use-scenery alike, since both drive the same companion action
// queue -- are queued and dispatched one at a time instead, through one
// shared queue so the two kinds can't race each other either.
enum CoopCompanionActionKind {
    COOP_COMPANION_ACTION_PICKUP,
    COOP_COMPANION_ACTION_USE,
    COOP_COMPANION_ACTION_SKILL,
};

struct CoopCompanionActionRequest {
    CoopCompanionActionKind kind;
    CoopItemEvent target;
    int32_t skill; // only meaningful for COOP_COMPANION_ACTION_SKILL
    bool targetIsHostDude; // only meaningful for COOP_COMPANION_ACTION_SKILL -- see CoopSkillRequest's comment
};

const int kCoopActionQueueCapacity = 16;
static CoopCompanionActionRequest g_coopActionQueue[kCoopActionQueueCapacity];
static int g_coopActionQueueHead = 0;
static int g_coopActionQueueLen = 0;
static bool g_coopCompanionActionBusy = false;
static uint32_t g_coopCompanionActionStartMs = 0;

// Safety net only -- cleared normally as soon as the companion's action
// actually succeeds (coopnet_notify_item_picked_up() for pickups; use
// requests have no completion signal yet, see coopnet_host_process_action_queue).
// Covers the case where the target can't actually be reached/used at all
// (unreachable path, already gone), which has no other completion signal.
const uint32_t kCoopCompanionActionTimeoutMs = 3000;

// ---------------------------------------------------------------------------
// Platform socket helpers
// ---------------------------------------------------------------------------

static uint32_t coopnet_now_ms()
{
    return static_cast<uint32_t>(SDL_GetTicks());
}

static void coopnet_sockets_init()
{
    if (g_coopSocketsInitialized) {
        return;
    }

#ifdef _WIN32
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif

    g_coopSocketsInitialized = true;
}

static void coopnet_set_nonblocking(CoopSocket sock)
{
#ifdef _WIN32
    u_long mode = 1;
    ioctlsocket(sock, FIONBIO, &mode);
#else
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);
#endif
}

static void coopnet_close_socket(CoopSocket& sock)
{
    if (sock == COOP_INVALID_SOCKET) {
        return;
    }

#ifdef _WIN32
    closesocket(sock);
#else
    close(sock);
#endif

    sock = COOP_INVALID_SOCKET;
}

static bool coopnet_would_block()
{
#ifdef _WIN32
    return WSAGetLastError() == WSAEWOULDBLOCK;
#else
    return errno == EWOULDBLOCK || errno == EAGAIN;
#endif
}

// ---------------------------------------------------------------------------
// Message framing
// ---------------------------------------------------------------------------

static bool coopnet_send_message(CoopSocket sock, uint8_t type, const void* payload, uint16_t payloadLen)
{
    if (sock == COOP_INVALID_SOCKET) {
        return false;
    }

    // Confirmed via testing: this was hardcoded to 256 while every receive-
    // side payload buffer was sized off kCoopMaxMessagePayload (2048, bumped
    // for CoopDialogueState) -- sending anything over 256 bytes (e.g. a
    // dialogue state) silently overflowed this stack buffer, corrupting the
    // stack and crashing the host with STATUS_STACK_BUFFER_OVERRUN
    // (0xc0000409, confirmed via Windows Event Viewer). Must stay in sync
    // with the receive-side buffers.
    if (payloadLen > kCoopMaxMessagePayload) {
        debug_printf("\nCoop: coopnet_send_message: payloadLen=%d exceeds kCoopMaxMessagePayload=%d, refusing to send type=%d\n",
            payloadLen, kCoopMaxMessagePayload, type);
        return false;
    }

    unsigned char buf[sizeof(CoopMsgHeader) + kCoopMaxMessagePayload];
    CoopMsgHeader header;
    header.length = payloadLen;
    header.type = type;

    memcpy(buf, &header, sizeof(header));
    if (payloadLen > 0) {
        memcpy(buf + sizeof(header), payload, payloadLen);
    }

    int total = sizeof(header) + payloadLen;
    int sent = 0;
    uint32_t startMs = coopnet_now_ms();
    while (sent < total) {
        int rc = send(sock, reinterpret_cast<const char*>(buf) + sent, total - sent, 0);
        if (rc <= 0) {
            if (coopnet_would_block()) {
                if (coopnet_now_ms() - startMs > kCoopSendTimeoutMs) {
                    debug_printf("\nCoop: send() timed out (buffer never drained), dropping message type=%d\n", type);
                    return false;
                }
                // Yield instead of busy-spinning -- see kCoopSendTimeoutMs's
                // comment for why an unbounded retry here is dangerous.
                SDL_Delay(1);
                continue;
            }
            return false;
        }
        sent += rc;
    }

    return true;
}

// Tries to pull one complete message out of the accumulated receive buffer for
// `sock`. Performs at most one non-blocking recv() call per invocation; caller
// should call this in a loop until it returns false to drain everything
// available in a single poll tick.
static bool coopnet_try_recv_message(CoopSocket sock, uint8_t* outType, unsigned char* outPayload, uint16_t* outPayloadLen)
{
    if (sock == COOP_INVALID_SOCKET) {
        return false;
    }

    int space = static_cast<int>(sizeof(g_coopRecvBuffer)) - g_coopRecvBufferLen;
    if (space > 0) {
        int rc = recv(sock, reinterpret_cast<char*>(g_coopRecvBuffer) + g_coopRecvBufferLen, space, 0);
        if (rc > 0) {
            debug_printf("\nCoop: recv() got %d bytes (bufferLen was %d)\n", rc, g_coopRecvBufferLen);
            g_coopRecvBufferLen += rc;
        } else if (rc == 0) {
            debug_printf("\nCoop: recv() returned 0 (peer closed connection)\n");
            // Orderly shutdown by peer.
            return false;
        } else if (!coopnet_would_block()) {
#ifdef _WIN32
            debug_printf("\nCoop: recv() error, WSAGetLastError=%d\n", WSAGetLastError());
#else
            debug_printf("\nCoop: recv() error, errno=%d\n", errno);
#endif
            return false;
        }
    }

    if (g_coopRecvBufferLen < static_cast<int>(sizeof(CoopMsgHeader))) {
        return false;
    }

    CoopMsgHeader header;
    memcpy(&header, g_coopRecvBuffer, sizeof(header));

    int totalNeeded = sizeof(header) + header.length;
    if (g_coopRecvBufferLen < totalNeeded) {
        return false;
    }

    *outType = header.type;
    *outPayloadLen = header.length;
    if (header.length > 0) {
        memcpy(outPayload, g_coopRecvBuffer + sizeof(header), header.length);
    }

    int remaining = g_coopRecvBufferLen - totalNeeded;
    if (remaining > 0) {
        memmove(g_coopRecvBuffer, g_coopRecvBuffer + totalNeeded, remaining);
    }
    g_coopRecvBufferLen = remaining;

    return true;
}

// ---------------------------------------------------------------------------
// Companion spawn/find (shared by host and client)
// ---------------------------------------------------------------------------

// Finds an existing party member sharing `pid` (other than obj_dude itself),
// or spawns a fresh one near (tile, elevation).
//
// NOTE: partyMemberAdd() is deliberately NOT called here. Its de-duplication
// check (party.cc) compares by `pid` alone ("partyMember->object->pid ==
// object->pid"), and since the companion intentionally reuses obj_dude's own
// pid (index 0 in the party list is always obj_dude), that check always
// matches obj_dude and silently no-ops the add. Confirmed via testing: this
// caused every coopnet_start_host() call to spawn a brand new loose companion
// object instead of finding the previous one (never actually registered),
// which is why pressing the host hotkey twice produced two visible
// companions. Real party registration (formation, combat team assignment)
// is deferred — known limitation, revisit before combat work.
static Object* coopnet_find_or_spawn_companion(int pid, int tile, int elevation)
{
    Object* existing = partyMemberFindObjFromPidStartingAt(pid, 1);
    if (existing != NULL) {
        debug_printf("\nCoop: found existing companion object (pid=%d)\n", pid);
        return existing;
    }

    Object* companion = NULL;
    if (obj_pid_new(&companion, pid) == -1) {
        debug_printf("\nCoop: obj_pid_new failed for pid=%d\n", pid);
        return NULL;
    }

    Rect rect;
    obj_move_to_tile(companion, tile, elevation, &rect);
    tile_refresh_rect(&rect, elevation);

    // OBJECT_NO_SAVE matters just as much as OBJECT_NO_REMOVE here, and was
    // missing until this was confirmed via testing: real party members get
    // both flags (party.cc's partyMemberAdd()), but the companion previously
    // only got OBJECT_NO_REMOVE. obj_save() (object.cc) skips anything with
    // OBJECT_NO_SAVE -- without it, the companion got written into every
    // map's own .SAV file on the way out (map_save_in_game(), called on
    // every transition), and revisiting that map later reloaded that stale
    // saved copy *alongside* the fresh one this function spawns -- the
    // "extra companion model" ghost seen after transitioning back to an
    // already-visited map.
    companion->flags |= (OBJECT_NO_REMOVE | OBJECT_NO_SAVE);

    debug_printf("\nCoop: spawned new companion object (pid=%d, tile=%d)\n", pid, tile);

    return companion;
}

// Shared by host and client: properly frees a companion object that is about
// to be replaced (e.g. after a map transition). obj_erase_object()/obj_remove()
// both silently refuse to touch anything with OBJECT_NO_REMOVE set (that's
// the whole point of the flag -- see coopnet_find_or_spawn_companion()), so
// it must be cleared first. Without this, map_load_file()'s obj_remove_all()
// leaves the old companion fully intact and still linked into whatever tile
// slot it last occupied, which then bleeds through onto the newly loaded
// map as a ghost object (same class of bug as the earlier item-ghosting
// issue) instead of ever actually being removed.
static void coopnet_destroy_companion(Object* companion)
{
    if (companion == NULL) {
        return;
    }

    // Capture before the call below -- obj_erase_object() frees the object
    // on success, so reading these fields afterward would be a use-after-free
    // (present in an earlier version of this function; never actually
    // crashed in testing, but was relying on freed memory not being reused
    // yet, purely by luck).
    int pid = companion->pid;
    int tile = companion->tile;
    int elevation = companion->elevation;

    companion->flags &= ~OBJECT_NO_REMOVE;
    int rc = obj_erase_object(companion, NULL);
    debug_printf("\nCoop-debug: coopnet_destroy_companion pid=%d tile=%d elevation=%d obj_erase_object rc=%d\n",
        pid, tile, elevation, rc);
}

// Shared by host and client: destroys the current companion and spawns a
// fresh one at (tile, elevation) -- same as calling
// coopnet_destroy_companion() + coopnet_find_or_spawn_companion() directly,
// except it also carries the old companion's inventory across the respawn.
// Map transitions force this respawn (coopnet_destroy_companion()'s comment
// explains why the companion can't just survive map_load_file() in place);
// without this, every transition silently wiped the companion's inventory,
// since the fresh spawn always starts empty. On the host this matters for
// real (it's the authoritative inventory, broadcast to the client right
// after); on the client it's a nice-to-have that avoids a brief flash of
// "empty inventory" before the next COOP_MSG_COMPANION_INVENTORY corrects it.
static Object* coopnet_respawn_companion(Object* oldCompanion, int pid, int tile, int elevation)
{
    CoopInventoryItemEntry saved[kCoopMaxInventorySyncItems];
    int savedCount = 0;

    if (oldCompanion != NULL) {
        Inventory* inventory = &(oldCompanion->data.inventory);
        savedCount = inventory->length;
        if (savedCount > kCoopMaxInventorySyncItems) {
            savedCount = kCoopMaxInventorySyncItems;
        }
        for (int i = 0; i < savedCount; i++) {
            saved[i].pid = inventory->items[i].item->pid;
            saved[i].quantity = inventory->items[i].quantity;
        }
    }

    coopnet_destroy_companion(oldCompanion);
    Object* newCompanion = coopnet_find_or_spawn_companion(pid, tile, elevation);

    if (newCompanion != NULL) {
        for (int i = 0; i < savedCount; i++) {
            Object* item = NULL;
            if (obj_pid_new(&item, saved[i].pid) == -1) {
                debug_printf("\nCoop: failed to restore companion item pid=%d after respawn\n", saved[i].pid);
                continue;
            }
            item_add_force(newCompanion, item, saved[i].quantity);

            // See coopnet_apply_companion_inventory()'s comment on the exact
            // same call -- without this, the restored item stays linked in
            // floatingObjects as well as the inventory, and the *next* map
            // transition's obj_remove_all() destroys it out from under the
            // inventory, corrupting the heap.
            obj_disconnect(item, NULL);
        }
        debug_printf("\nCoop: respawned companion, restored %d inventory stack(s)\n", savedCount);
    }

    return newCompanion;
}

// Shared by host and client, called every tick: any companion inventory item
// that has its own script needs SCRIPT_FLAG_0x08|0x10 set on it, or the very
// next map transition's cleanup (scr_remove_all(), called at the start of
// every obj_remove_all()) destroys the item's script out from under it while
// the item Object itself survives via the companion's inventory -- confirmed
// via testing: crashed with STATUS_ACCESS_VIOLATION (0xc0000005, via Windows
// Event Viewer) on the transition right after the companion picked up a
// scripted item, even after fixing the earlier floatingObjects double-
// reference bug (see coopnet_apply_companion_inventory()'s comment). This is
// exactly the protection party.cc's partyMemberPrepItemSaveAll() gives real
// party members' items before a transition -- the companion isn't a real
// party member (see coopnet_find_or_spawn_companion()'s comment) so it gets
// none of that automatically. Applied continuously (every tick) rather than
// only right before a transition, since -- unlike the client's own explicit
// COOP_MSG_MAP_TRANSITION -- a host-triggered transition runs through
// vanilla's own object.cc/map.cc code with no hook point available before
// map_load() actually happens.
static void coopnet_protect_companion_item_scripts()
{
    if (g_coopCompanion == NULL) {
        return;
    }

    Inventory* inventory = &(g_coopCompanion->data.inventory);
    for (int i = 0; i < inventory->length; i++) {
        Object* item = inventory->items[i].item;
        if (item == NULL || item->sid == -1) {
            continue;
        }

        Script* script;
        if (scr_ptr(item->sid, &script) == -1) {
            continue;
        }

        script->scr_flags |= (SCRIPT_FLAG_0x08 | SCRIPT_FLAG_0x10);
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

#ifdef _WIN32
// Windows automatically throttles CPU scheduling for background/unfocused
// windows to save power ("Efficiency Mode"/EcoQoS), independent of anything
// this process does internally. Confirmed via testing to still cause slow
// catch-up and visual staleness for a backgrounded coop-debug window even
// after the engine's own GNW95_lost_focus block was bypassed. Explicitly
// opt this process out of that throttling and bump its scheduling priority
// slightly so both windows keep processing coop traffic promptly regardless
// of focus. Only applied in coop-debug mode; irrelevant for real two-machine
// play where each player's window stays focused on their own screen.
static void coopnet_disable_background_throttling()
{
    SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);

#if defined(PROCESS_POWER_THROTTLING_EXECUTION_SPEED)
    PROCESS_POWER_THROTTLING_STATE state;
    memset(&state, 0, sizeof(state));
    state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    state.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    state.StateMask = 0; // 0 = do not throttle
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &state, sizeof(state));
#endif
}
#endif

void coopnet_parse_command_line(int argc, char** argv)
{
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--coop-debug") == 0) {
            g_coopAllowMultipleInstances = true;
        } else if (strncmp(argv[i], "--coop-name=", 12) == 0) {
            strncpy(g_coopInstanceLabel, argv[i] + 12, sizeof(g_coopInstanceLabel) - 1);
        } else if (strncmp(argv[i], "--coop-connect=", 15) == 0) {
            strncpy(g_coopConnectTarget, argv[i] + 15, sizeof(g_coopConnectTarget) - 1);
        }
    }

    if (g_coopAllowMultipleInstances) {
        // debug_register_env()/_log()/_mono() are never called anywhere else
        // in this codebase, so debug_printf() is normally a silent no-op.
        // Wire up file logging ourselves so coop debug output is actually
        // visible somewhere during development.
        debug_register_log("coopnet_debug.log", "wt");

#ifdef _WIN32
        coopnet_disable_background_throttling();
#endif
    }
}

bool coopnet_allow_multiple_instances()
{
    return g_coopAllowMultipleInstances;
}

const char* coopnet_get_instance_label()
{
    return g_coopInstanceLabel;
}

const char* coopnet_get_connect_target()
{
    return g_coopConnectTarget;
}

bool coopnet_start_host(int port)
{
    if (g_coopRole == CoopRole::Host) {
        debug_printf("\nCoop: already hosting, ignoring repeated request\n");
        return true;
    }

    coopnet_sockets_init();

    g_coopCompanion = coopnet_find_or_spawn_companion(obj_dude->pid, obj_dude->tile, obj_dude->elevation);
    if (g_coopCompanion == NULL) {
        return false;
    }

    strncpy(g_coopHostLastMapName, map_data.name, sizeof(g_coopHostLastMapName) - 1);
    g_coopHostLastMapName[sizeof(g_coopHostLastMapName) - 1] = '\0';
    g_coopCompanionGameOverSent = false;

    CoopSocket listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSocket == COOP_INVALID_SOCKET) {
        return false;
    }

    int reuse = 1;
    setsockopt(listenSocket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(static_cast<uint16_t>(port));

    if (bind(listenSocket, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        coopnet_close_socket(listenSocket);
        return false;
    }

    if (listen(listenSocket, 1) != 0) {
        coopnet_close_socket(listenSocket);
        return false;
    }

    coopnet_set_nonblocking(listenSocket);

    g_coopListenSocket = listenSocket;
    g_coopRole = CoopRole::Host;
    g_coopConnState = CoopConnState::Listening;
    g_coopLastFollowCheckTimeMs = coopnet_now_ms();
    g_coopLastCommandedTile[0] = -1;

    return true;
}

bool coopnet_start_client(const char* ip, int port)
{
    if (g_coopRole == CoopRole::Client && g_coopConnState != CoopConnState::Idle) {
        debug_printf("\nCoop: already connecting/connected, ignoring repeated request\n");
        return true;
    }

    coopnet_sockets_init();

    CoopSocket sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == COOP_INVALID_SOCKET) {
        return false;
    }

    coopnet_set_nonblocking(sock);

    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) {
        coopnet_close_socket(sock);
        return false;
    }

    connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    // Non-blocking connect: expected to return immediately with an
    // in-progress error; actual completion is detected in coopnet_poll().

    g_coopPeerSocket = sock;
    g_coopRole = CoopRole::Client;
    g_coopConnState = CoopConnState::Connecting;
    g_coopRecvBufferLen = 0;
    g_coopCompanion = NULL;
    g_coopLastCommandedTile[0] = -1;
    g_coopLastCommandedTile[1] = -1;

    return true;
}

// Client-side only, defined further down alongside the rest of the
// dialogue-mirror window code -- forward-declared here so coopnet_shutdown()
// can make sure it never lingers on screen after a disconnect.
static void coopnet_close_dialogue_window();

void coopnet_shutdown()
{
    coopnet_close_socket(g_coopListenSocket);
    coopnet_close_socket(g_coopPeerSocket);
    g_coopRole = CoopRole::None;
    g_coopConnState = CoopConnState::Idle;
    g_coopCompanion = NULL;
    g_coopRecvBufferLen = 0;
    g_coopLastCommandedTile[0] = -1;
    g_coopLastCommandedTile[1] = -1;
    coopnet_close_dialogue_window();
}

CoopRole coopnet_get_role()
{
    return g_coopRole;
}

bool coopnet_is_connected()
{
    return g_coopConnState == CoopConnState::Connected;
}

Object* coopnet_get_companion()
{
    return g_coopCompanion;
}

static void coopnet_host_apply_move_intent(const CoopMoveIntent& intent)
{
    if (g_coopCompanion == NULL) {
        return;
    }

    if (isInCombat()) {
        // Combat movement must go through coopnet_combat_input()'s
        // turn-gated COOP_MSG_COMBAT_ACTION path instead -- applying a plain
        // move-intent here would let the client move the companion for
        // free, out of turn order.
        return;
    }

    if (g_coopLastCommandedTile[0] == intent.targetTile) {
        // Already heading there (e.g. a repeated click while still running
        // toward the same spot) — don't restart the animation for nothing.
        return;
    }

    // Unlike the client's periodic position-apply (which waits for real
    // arrival before redirecting — see coopnet_client_apply_position), a
    // move-intent here represents a discrete, deliberate new click. Vanilla
    // interrupts an in-progress walk immediately on a new click too (see
    // check_move()'s register_clear(obj_dude) for "interrupt walk"), so this
    // does the same rather than waiting — throttling it by time made the
    // companion feel less responsive to clicks than the vanilla player.
    register_clear(g_coopCompanion);
    register_begin(ANIMATION_REQUEST_UNRESERVED);
    register_object_run_to_tile(g_coopCompanion, intent.targetTile, g_coopCompanion->elevation, -1, 0);
    register_end();
    g_coopLastCommandedTile[0] = intent.targetTile;
}

// Host-side only: applies a COOP_MSG_COMBAT_ACTION move during the
// companion's own combat turn -- see g_coopHostCombatTurnActive's comment
// for why this is only ever called while that's true. AP-gated the same way
// combat_ai()'s own movement is (e.g. action_get_an_object()'s isInCombat()
// branch passes the mover's current combat.ap as the budget) -- the
// underlying register_object_move_to_tile() deducts consumed AP internally.
static void coopnet_host_apply_combat_move(int targetTile)
{
    if (g_coopCompanion == NULL) {
        return;
    }

    register_clear(g_coopCompanion);
    register_begin(ANIMATION_REQUEST_RESERVED);
    register_object_move_to_tile(g_coopCompanion, targetTile, g_coopCompanion->elevation, g_coopCompanion->data.critter.combat.ap, 0);
    register_end();
}

// Host-side only: applies a COOP_MSG_COMBAT_ACTION attack during the
// companion's own combat turn. Reuses combat_ai() -- the same function that
// drives a full AI-controlled turn elsewhere in combat_turn() -- rather than
// combat_attack_this()'s player-specific flow (which is deeply tied to
// obj_dude: it reads the interface's weapon/aim-mode selection and reports
// out-of-ammo/out-of-range/etc. via on-screen player messages, none of which
// make sense for the companion). combat_ai(critter, target) already handles
// both cases cleanly: given a specific target it calls ai_try_attack()
// directly; given NULL it picks one itself via ai_danger_source() first.
// Either way it also handles moving into range and weapon selection
// automatically, so one press can consume the rest of the turn's AP on
// whatever it decides, not a single precise attack.
static void coopnet_host_apply_combat_attack(int32_t targetId)
{
    if (g_coopCompanion == NULL) {
        return;
    }

    Object* target = NULL;
    if (targetId != -1) {
        int count = combat_get_list_count();
        for (int i = 0; i < count; i++) {
            Object* candidate = combat_get_list_item(i);
            if (candidate != NULL && candidate->id == targetId) {
                target = candidate;
                break;
            }
        }
        if (target == NULL) {
            debug_printf("\nCoop: attack targetId=%d not found (already dead/gone?), falling back to auto-target\n", targetId);
        }
    }

    combat_ai(g_coopCompanion, target);
}

static void coopnet_host_broadcast_positions()
{
    if (g_coopCompanion != NULL) {
        CoopPosition pos;
        pos.which = 0;
        pos.tile = g_coopCompanion->tile;
        pos.elevation = g_coopCompanion->elevation;
        pos.rotation = g_coopCompanion->rotation;
        coopnet_send_message(g_coopPeerSocket, COOP_MSG_POSITION, &pos, sizeof(pos));
    }

    if (obj_dude != NULL) {
        CoopPosition pos;
        pos.which = 1;
        pos.tile = obj_dude->tile;
        pos.elevation = obj_dude->elevation;
        pos.rotation = obj_dude->rotation;
        coopnet_send_message(g_coopPeerSocket, COOP_MSG_POSITION, &pos, sizeof(pos));
    }
}

// Host-side only, called alongside coopnet_host_broadcast_positions() at the
// same cadence while a synced combat is active: broadcasts every combat
// participant's current state, skipping obj_dude and the companion (already
// covered by position broadcasts above). No-op outside combat.
// combat_get_list_count()/_item() are small read-only accessors onto
// combat.cc's otherwise-file-static combat_list[].
static void coopnet_host_broadcast_combat_participants()
{
    if (!isInCombat()) {
        return;
    }

    int count = combat_get_list_count();
    for (int i = 0; i < count; i++) {
        Object* critter = combat_get_list_item(i);
        if (critter == NULL || critter == obj_dude || critter == g_coopCompanion) {
            continue;
        }

        CoopCombatParticipant participant;
        participant.id = critter->id;
        participant.pid = critter->pid;
        participant.tile = critter->tile;
        participant.elevation = critter->elevation;
        participant.rotation = critter->rotation;
        participant.hp = stat_level(critter, STAT_CURRENT_HIT_POINTS);
        participant.isDead = critter_is_dead(critter) ? 1 : 0;
        debug_printf("\nCoop-debug: broadcasting participant id=%d pid=%d tile=%d hp=%d isDead=%d (listIndex=%d/%d)\n",
            participant.id, participant.pid, participant.tile, participant.hp, participant.isDead, i, count);
        coopnet_send_message(g_coopPeerSocket, COOP_MSG_COMBAT_PARTICIPANT, &participant, sizeof(participant));
    }
}

// Host-side only: sends a full snapshot of the companion's current inventory
// -- see the CoopInventorySync comment above for why this exists and why
// it's a full snapshot rather than a diff.
static void coopnet_host_broadcast_companion_inventory()
{
    if (g_coopCompanion == NULL) {
        return;
    }

    Inventory* inventory = &(g_coopCompanion->data.inventory);

    CoopInventorySync sync;
    int count = inventory->length;
    if (count > kCoopMaxInventorySyncItems) {
        debug_printf("\nCoop: companion inventory has %d items, only syncing first %d\n", count, kCoopMaxInventorySyncItems);
        count = kCoopMaxInventorySyncItems;
    }

    sync.itemCount = static_cast<uint8_t>(count);
    for (int i = 0; i < count; i++) {
        sync.items[i].pid = inventory->items[i].item->pid;
        sync.items[i].quantity = inventory->items[i].quantity;
    }

    bool sent = coopnet_send_message(g_coopPeerSocket, COOP_MSG_COMPANION_INVENTORY, &sync, sizeof(sync));
    debug_printf("\nCoop: broadcast companion inventory (%d items) success=%d\n", count, sent);
}

// Host-side only, called every tick regardless of connection state (the
// companion still needs to follow through transitions during solo play,
// same reasoning as coopnet_host_run_disconnected_follow()). Detects the
// host's own map having changed -- a normal exit/elevator was used, or
// worldmap travel finished loading a new area -- by comparing map_data.name
// against what it was last tick. Nothing else keeps the client in step
// across a map change, since the client runs its own fully independent copy
// of the game with its own map loaded: it never sees object.cc's exit-tile
// check fire for the host's own dude. Also respawns the companion here,
// since it is not a real party member (see coopnet_find_or_spawn_companion()'s
// comment) and does not survive map_load_file()'s obj_remove_all() cleanly
// (see coopnet_destroy_companion()'s comment).
static void coopnet_host_check_map_transition()
{
    if (strncmp(g_coopHostLastMapName, map_data.name, sizeof(g_coopHostLastMapName)) == 0) {
        return;
    }

    strncpy(g_coopHostLastMapName, map_data.name, sizeof(g_coopHostLastMapName) - 1);
    g_coopHostLastMapName[sizeof(g_coopHostLastMapName) - 1] = '\0';

    if (map_data.name[0] == '\0') {
        // map_data.name goes transiently empty while the host is just
        // browsing the worldmap screen (before picking a destination) --
        // confirmed via testing: coopnet_poll() runs inside world_map()'s
        // own loop (added so the connection survives long travel times),
        // so this check runs during that transient window too. Not a real
        // transition -- nothing to relay yet. The tracked name above is
        // still updated, so the *next* real change (this empty state ->
        // the actual destination map, once travel finishes) is still
        // caught normally. Without this guard, a bogus empty-name
        // MAP_TRANSITION got sent to the client, immediately followed by
        // the real one -- confusing enough that the client ended up
        // somewhere unexpected (a random worldmap encounter) instead of
        // wherever the host actually intended to go.
        return;
    }

    debug_printf("\nCoop: host map changed to %.16s, respawning companion (old companion=%p pid=%d tile=%d)\n",
        map_data.name, (void*)g_coopCompanion, g_coopCompanion != NULL ? g_coopCompanion->pid : -1, g_coopCompanion != NULL ? g_coopCompanion->tile : -1);

    g_coopCompanion = coopnet_respawn_companion(g_coopCompanion, obj_dude->pid, obj_dude->tile, obj_dude->elevation);

    debug_printf("\nCoop-debug: new companion=%p pid=%d tile=%d\n",
        (void*)g_coopCompanion, g_coopCompanion != NULL ? g_coopCompanion->pid : -1, g_coopCompanion != NULL ? g_coopCompanion->tile : -1);

    g_coopLastCommandedTile[0] = -1;
    g_coopLastCommandedTile[1] = -1;

    if (g_coopConnState != CoopConnState::Connected) {
        return;
    }

    CoopMapTransition transition;
    memset(&transition, 0, sizeof(transition));
    strncpy(transition.mapName, map_data.name, sizeof(transition.mapName) - 1);
    transition.tile = obj_dude->tile;
    transition.elevation = obj_dude->elevation;
    transition.rotation = obj_dude->rotation;

    bool sent = coopnet_send_message(g_coopPeerSocket, COOP_MSG_MAP_TRANSITION, &transition, sizeof(transition));
    debug_printf("\nCoop: sent MAP_TRANSITION to %.16s (tile=%d elevation=%d) success=%d\n",
        transition.mapName, transition.tile, transition.elevation, sent);

    if (kCoopCompanionInventorySyncEnabled) {
        coopnet_host_broadcast_companion_inventory();
    }
}

// Host-side only, called every tick from coopnet_poll_host(): the companion
// dying is a shared game over by design (a deliberate project decision, not
// default engine behavior -- vanilla has no concept of "the companion" at
// all), same as obj_dude's own death already is (see main.cc). Ends the
// host's own game exactly the same way -- game_user_wants_to_quit, checked
// by main_game_loop()'s own while condition -- and tells the client first.
static void coopnet_host_check_companion_death()
{
    if (g_coopCompanionGameOverSent || g_coopCompanion == NULL) {
        return;
    }

    if (!critter_is_dead(g_coopCompanion)) {
        return;
    }

    g_coopCompanionGameOverSent = true;
    debug_printf("\nCoop: companion died, ending shared game\n");
    coopnet_notify_game_over(COOP_GAME_OVER_COMPANION_DIED);

    // Same mechanism obj_dude's own death uses (main.cc) -- confirmed via
    // testing that plain game_user_wants_to_quit = 1 alone (the original
    // version of this fix) ended the host's game but skipped the real death
    // cutscene/narration entirely. Using exactly 2 (not another nonzero
    // value) also matters: mainmenu.cc and combat.cc's own
    // game_user_wants_to_quit checks are tuned for the values obj_dude's
    // real death already uses, so reusing 2 here reuses that
    // already-working path instead of exercising an untested combination.
    main_request_death_scene();
    game_user_wants_to_quit = 2;
}

// Crude placeholder follow-AI used only while no client is connected, so the
// host can keep playing solo instead of the companion standing frozen. Real
// follow behavior (formation offsets, obstacle avoidance) is future work.
static void coopnet_host_run_disconnected_follow()
{
    if (g_coopCompanion == NULL || obj_dude == NULL) {
        return;
    }

    uint32_t now = coopnet_now_ms();
    if (now - g_coopLastFollowCheckTimeMs < kCoopFollowCheckIntervalMs) {
        return;
    }
    g_coopLastFollowCheckTimeMs = now;

    if (g_coopCompanion->elevation != obj_dude->elevation) {
        return;
    }

    int distance = tile_dist(g_coopCompanion->tile, obj_dude->tile);
    if (distance > kCoopFollowDistanceThreshold) {
        register_clear(g_coopCompanion);
        register_begin(ANIMATION_REQUEST_UNRESERVED);
        register_object_run_to_tile(g_coopCompanion, obj_dude->tile, g_coopCompanion->elevation, -1, 0);
        register_end();
    }
}

// Shared by both host and client: applying a peer's item drop/pickup to our
// own world. See the (pid, tile, elevation)-identity caveat in coopnet.h.
//
// Both call tile_refresh_display() (a full-viewport redraw) on top of the
// normal per-rect refresh -- confirmed via testing that the narrow rect
// refresh alone could leave a stale "ghost" copy of the item's sprite
// on screen (especially right around a camera scroll), the same class of
// partial-redraw artifact already seen with companion movement. Item events
// are rare (not a 10x/second path like position broadcasts), so the extra
// cost of a full redraw here is not worth optimizing away.

static void coopnet_apply_item_dropped(const CoopItemEvent& evt)
{
    Object* item = NULL;
    if (obj_pid_new(&item, evt.pid) == -1) {
        debug_printf("\nCoop: obj_pid_new failed applying item drop (pid=%d)\n", evt.pid);
        return;
    }

    // obj_pid_new() (via obj_new()'s obj_insert()) already links the fresh
    // item into floatingObjects. obj_connect() -- used here previously --
    // creates a brand new ObjectListNode and never removes that original
    // one, leaving the item double-linked (floatingObjects *and*
    // objectTable[tile]). The next obj_remove_all() (any map transition)
    // then frees it twice -- a double-free, confirmed via testing:
    // STATUS_HEAP_CORRUPTION (0xc0000374, via Windows Event Viewer) on the
    // transition right after any item drop got synced to the client, with
    // no pickup involved at all. obj_move_to_tile() is the correct function
    // for an object that already has a node somewhere (it looks up and
    // reuses the existing one via obj_node_ptr() instead of creating a
    // second one) -- same reasoning as coopnet_find_or_spawn_companion()
    // already using it instead of obj_connect() for the same reason.
    Rect rect;
    obj_move_to_tile(item, evt.tile, evt.elevation, &rect);
    tile_refresh_rect(&rect, evt.elevation);
    tile_refresh_display();
}

static void coopnet_apply_item_picked_up(const CoopItemEvent& evt)
{
    for (Object* object = obj_find_first_at(evt.elevation); object != NULL; object = obj_find_next_at()) {
        if (object->tile == evt.tile && object->pid == evt.pid) {
            obj_destroy(object);
            tile_refresh_display();
            return;
        }
    }

    debug_printf("\nCoop: item pickup notification had no matching ground item (pid=%d, tile=%d)\n", evt.pid, evt.tile);
}

// Client-side only: replaces the client's local companion mirror's inventory
// wholesale with the host's snapshot. A full clear-then-rebuild rather than
// an in-place diff -- simpler and can't drift.
//
// The clear step uses obj_inven_free(), the same utility the engine itself
// uses to tear down a critter's/container's whole inventory (e.g. on
// destruction) -- an earlier version of this hand-rolled the clear with
// item_remove_mult()+obj_erase_object() per item instead, which is what
// produced the floating stray item sprites seen during testing (dropping an
// item, or having one arrive via a stale/overlapping sync, left a corrupted
// object behind instead of being cleanly freed).
static void coopnet_apply_companion_inventory(const CoopInventorySync& sync)
{
    if (g_coopCompanion == NULL) {
        return;
    }

    Inventory* inventory = &(g_coopCompanion->data.inventory);
    obj_inven_free(inventory);

    for (int i = 0; i < sync.itemCount; i++) {
        Object* newItem = NULL;
        if (obj_pid_new(&newItem, sync.items[i].pid) == -1) {
            debug_printf("\nCoop: obj_pid_new failed applying companion inventory sync (pid=%d)\n", sync.items[i].pid);
            continue;
        }
        item_add_force(g_coopCompanion, newItem, sync.items[i].quantity);

        // obj_pid_new() (via obj_new()'s obj_insert()) links the fresh item
        // into floatingObjects; item_add_force() only wires it into the
        // inventory array, it never unlinks it from there (confirmed by
        // reading protinst.cc's real item-pickup flow, which calls
        // obj_disconnect() itself right after item_add_mult() for exactly
        // this reason). Without this, the item stays double-referenced --
        // still in floatingObjects *and* in the inventory -- and the next
        // obj_remove_all() (any map transition) destroys it out from under
        // the inventory that still points to it, corrupting the heap.
        // Confirmed via testing: crashed with STATUS_HEAP_CORRUPTION
        // (0xc0000374, via Windows Event Viewer) on the map transition right
        // after a synced item first made it into the companion's inventory.
        obj_disconnect(newItem, NULL);
    }

    debug_printf("\nCoop: applied companion inventory sync (%d items)\n", sync.itemCount);
}

// Host-side only: enqueues a client's action request (pickup, use, or skill)
// rather than applying it immediately -- see the g_coopActionQueue comment
// above for why. `skill` is only meaningful for COOP_COMPANION_ACTION_SKILL.
static void coopnet_enqueue_companion_action(CoopCompanionActionKind kind, const CoopItemEvent& evt, int32_t skill = -1, bool targetIsHostDude = false)
{
    if (g_coopActionQueueLen >= kCoopActionQueueCapacity) {
        debug_printf("\nCoop: companion action queue full, dropping request (kind=%d, pid=%d, tile=%d)\n", kind, evt.pid, evt.tile);
        return;
    }

    int tail = (g_coopActionQueueHead + g_coopActionQueueLen) % kCoopActionQueueCapacity;
    g_coopActionQueue[tail].kind = kind;
    g_coopActionQueue[tail].target = evt;
    g_coopActionQueue[tail].skill = skill;
    g_coopActionQueue[tail].targetIsHostDude = targetIsHostDude;
    g_coopActionQueueLen++;
}

// Host-side only, called once per poll tick: dispatches at most one queued
// action request, running the real action (movement-to-target, animation,
// is_next_to check) against the authoritative companion object -- the same
// action_get_an_object()/action_use_an_object() paths vanilla uses for
// obj_dude's own clicks.
//
// For pickups, obj_pickup's existing coopnet_notify_item_picked_up() hook
// broadcasts completion back to the client as a normal COOP_MSG_ITEM_PICKED_UP
// (applied via coopnet_apply_item_picked_up like any other peer pickup), and
// also clears g_coopCompanionActionBusy so the next queued request can start
// right away.
//
// For use-scenery (e.g. doors), there is no such hook yet -- the companion
// will genuinely open the door on the host's authoritative world, but that
// doesn't yet get broadcast back to update the client's own independently-
// loaded copy of the same door object. g_coopCompanionActionBusy for a use
// request is only ever cleared by the safety timeout below. Known limitation,
// not silently worked around -- full scenery-state sync is a separate piece
// of work, same category as the general "simulation divergence" limitation
// documented for exploration mode.
static void coopnet_host_process_action_queue()
{
    if (g_coopHostCombatTurnActive) {
        // Pickup/use actions (action_get_an_object()/action_use_an_object())
        // and the companion's real combat turn both drive the companion's
        // shared register_begin()/register_end() animation queue -- Phase 2
        // of milestone 3 never accounted for the two interleaving. Left
        // queued rather than dropped: it'll dispatch once combat frees the
        // companion up again.
        return;
    }

    if (g_coopCompanionActionBusy) {
        if (coopnet_now_ms() - g_coopCompanionActionStartMs > kCoopCompanionActionTimeoutMs) {
            debug_printf("\nCoop: companion action timed out without completing (never reached target / never got picked up)\n");
            g_coopCompanionActionBusy = false;
        } else {
            return;
        }
    }

    if (g_coopActionQueueLen == 0 || g_coopCompanion == NULL) {
        return;
    }

    CoopCompanionActionRequest request = g_coopActionQueue[g_coopActionQueueHead];
    g_coopActionQueueHead = (g_coopActionQueueHead + 1) % kCoopActionQueueCapacity;
    g_coopActionQueueLen--;

    const CoopItemEvent& evt = request.target;

    // See CoopSkillRequest's comment: the companion shares obj_dude's own
    // pid, so a plain (pid, tile, elevation) search for "the host's
    // character" is ambiguous -- resolve straight to obj_dude instead of
    // searching at all.
    if (request.kind == COOP_COMPANION_ACTION_SKILL && request.targetIsHostDude) {
        debug_printf("\nCoop: dispatching companion action kind=%d (skill=%d) directly at obj_dude, companion tile=%d elevation=%d, dist=%d\n",
            request.kind, request.skill, g_coopCompanion->tile, g_coopCompanion->elevation, obj_dist(g_coopCompanion, obj_dude));
        g_coopCompanionActionBusy = true;
        g_coopCompanionActionStartMs = coopnet_now_ms();
        if (action_use_skill_on(g_coopCompanion, obj_dude, request.skill) == -1) {
            debug_printf("\nCoop: action_use_skill_on failed (skill=%d, target=obj_dude)\n", request.skill);
            g_coopCompanionActionBusy = false;
        }
        return;
    }

    // Skill targets can be an item, scenery, or critter depending on the
    // skill (Steal/First Aid/Doctor/Science/Repair all target critters,
    // Lockpick/Traps target items/scenery) -- unlike pickup/use, there's no
    // single expected FID_TYPE to filter by, so match on (pid, tile,
    // elevation) alone.
    int wantType = -1;
    if (request.kind == COOP_COMPANION_ACTION_PICKUP) {
        wantType = OBJ_TYPE_ITEM;
    } else if (request.kind == COOP_COMPANION_ACTION_USE) {
        wantType = OBJ_TYPE_SCENERY;
    }

    for (Object* object = obj_find_first_at(evt.elevation); object != NULL; object = obj_find_next_at()) {
        if (object->tile == evt.tile && object->pid == evt.pid && (wantType == -1 || FID_TYPE(object->fid) == wantType)) {
            debug_printf("\nCoop: dispatching companion action kind=%d, companion tile=%d elevation=%d, target tile=%d elevation=%d, dist=%d\n",
                request.kind, g_coopCompanion->tile, g_coopCompanion->elevation, object->tile, object->elevation, obj_dist(g_coopCompanion, object));
            g_coopCompanionActionBusy = true;
            g_coopCompanionActionStartMs = coopnet_now_ms();
            if (request.kind == COOP_COMPANION_ACTION_PICKUP) {
                action_get_an_object(g_coopCompanion, object);
            } else if (request.kind == COOP_COMPANION_ACTION_USE) {
                action_use_an_object(g_coopCompanion, object);
            } else {
                if (action_use_skill_on(g_coopCompanion, object, request.skill) == -1) {
                    debug_printf("\nCoop: action_use_skill_on failed (skill=%d)\n", request.skill);
                    g_coopCompanionActionBusy = false;
                }
            }
            return;
        }
    }

    debug_printf("\nCoop: queued companion action had no matching target (kind=%d, pid=%d, tile=%d)\n", request.kind, evt.pid, evt.tile);
}

static void coopnet_poll_host()
{
    coopnet_protect_companion_item_scripts();
    coopnet_host_check_map_transition();
    coopnet_host_check_companion_death();

    if (g_coopConnState == CoopConnState::Listening) {
        sockaddr_in clientAddr;
#ifdef _WIN32
        int addrLen = sizeof(clientAddr);
#else
        socklen_t addrLen = sizeof(clientAddr);
#endif
        CoopSocket accepted = accept(g_coopListenSocket, reinterpret_cast<sockaddr*>(&clientAddr), &addrLen);
        if (accepted != COOP_INVALID_SOCKET) {
            debug_printf("\nCoop: accepted client connection, waiting for HELLO\n");
            coopnet_set_nonblocking(accepted);
            g_coopPeerSocket = accepted;
            g_coopRecvBufferLen = 0;
            g_coopConnState = CoopConnState::WaitingForHello;
            g_coopLastRecvTimeMs = coopnet_now_ms();
        }
    }

    if (g_coopConnState == CoopConnState::WaitingForHello) {
        uint8_t type;
        unsigned char payload[kCoopMaxMessagePayload];
        uint16_t payloadLen;
        if (coopnet_try_recv_message(g_coopPeerSocket, &type, payload, &payloadLen)) {
            debug_printf("\nCoop: host received message type=%d len=%d while waiting for HELLO\n", type, payloadLen);
            if (type == COOP_MSG_HELLO && payloadLen == sizeof(CoopHello)) {
                CoopHello hello;
                memcpy(&hello, payload, sizeof(hello));
                debug_printf("\nCoop: HELLO received (protocolVersion=%u, mapName=%.16s)\n", hello.protocolVersion, hello.mapName);

                CoopHelloAck ack;
                ack.accepted = 1;
                ack.companionPid = g_coopCompanion != NULL ? g_coopCompanion->pid : -1;
                ack.companionTile = g_coopCompanion != NULL ? g_coopCompanion->tile : -1;
                ack.companionElevation = g_coopCompanion != NULL ? g_coopCompanion->elevation : 0;
                bool sent = coopnet_send_message(g_coopPeerSocket, COOP_MSG_HELLO_ACK, &ack, sizeof(ack));
                debug_printf("\nCoop: sent HELLO_ACK (companionPid=%d, companionTile=%d) success=%d -- now Connected\n", ack.companionPid, ack.companionTile, sent);
                g_coopConnState = CoopConnState::Connected;
                g_coopLastRecvTimeMs = coopnet_now_ms();
                g_coopLastBroadcastTimeMs = coopnet_now_ms();

                // So a reconnecting/late-joining client immediately sees
                // whatever the companion already has, not just future changes.
                if (kCoopCompanionInventorySyncEnabled) {
                    coopnet_host_broadcast_companion_inventory();
                }

                win_msg("Client connected!", 100, 100, 0);
            }
        }
    }

    if (g_coopConnState == CoopConnState::Connected) {
        bool disconnected = false;

        uint8_t type;
        unsigned char payload[kCoopMaxMessagePayload];
        uint16_t payloadLen;
        while (coopnet_try_recv_message(g_coopPeerSocket, &type, payload, &payloadLen)) {
            g_coopLastRecvTimeMs = coopnet_now_ms();
            if (type == COOP_MSG_MOVE_INTENT && payloadLen == sizeof(CoopMoveIntent)) {
                CoopMoveIntent intent;
                memcpy(&intent, payload, sizeof(intent));
                debug_printf("\nCoop: received MOVE_INTENT targetTile=%d (companion=%p)\n", intent.targetTile, (void*)g_coopCompanion);
                coopnet_host_apply_move_intent(intent);
            } else if (type == COOP_MSG_ITEM_DROPPED && payloadLen == sizeof(CoopItemEvent)) {
                CoopItemEvent evt;
                memcpy(&evt, payload, sizeof(evt));
                debug_printf("\nCoop: received ITEM_DROPPED from client (pid=%d, tile=%d)\n", evt.pid, evt.tile);
                coopnet_apply_item_dropped(evt);
            } else if (type == COOP_MSG_ITEM_PICKED_UP && payloadLen == sizeof(CoopItemEvent)) {
                CoopItemEvent evt;
                memcpy(&evt, payload, sizeof(evt));
                debug_printf("\nCoop: received ITEM_PICKED_UP from client (pid=%d, tile=%d)\n", evt.pid, evt.tile);
                coopnet_apply_item_picked_up(evt);
            } else if (type == COOP_MSG_PICKUP_REQUEST && payloadLen == sizeof(CoopItemEvent)) {
                CoopItemEvent evt;
                memcpy(&evt, payload, sizeof(evt));
                debug_printf("\nCoop: received PICKUP_REQUEST from client (pid=%d, tile=%d)\n", evt.pid, evt.tile);
                coopnet_enqueue_companion_action(COOP_COMPANION_ACTION_PICKUP, evt);
            } else if (type == COOP_MSG_USE_REQUEST && payloadLen == sizeof(CoopItemEvent)) {
                CoopItemEvent evt;
                memcpy(&evt, payload, sizeof(evt));
                debug_printf("\nCoop: received USE_REQUEST from client (pid=%d, tile=%d)\n", evt.pid, evt.tile);
                coopnet_enqueue_companion_action(COOP_COMPANION_ACTION_USE, evt);
            } else if (type == COOP_MSG_SKILL_REQUEST && payloadLen == sizeof(CoopSkillRequest)) {
                CoopSkillRequest req;
                memcpy(&req, payload, sizeof(req));
                debug_printf("\nCoop: received SKILL_REQUEST from client (skill=%d, pid=%d, tile=%d, targetIsHostDude=%d)\n", req.skill, req.pid, req.tile, req.targetIsHostDude);
                CoopItemEvent evt;
                evt.pid = req.pid;
                evt.tile = req.tile;
                evt.elevation = req.elevation;
                coopnet_enqueue_companion_action(COOP_COMPANION_ACTION_SKILL, evt, req.skill, req.targetIsHostDude != 0);
            } else if (type == COOP_MSG_COMBAT_ACTION && payloadLen == sizeof(CoopCombatAction)) {
                if (!g_coopHostCombatTurnActive) {
                    // Stray/late message outside the companion's actual
                    // turn window -- see g_coopHostCombatTurnActive's comment.
                    debug_printf("\nCoop: ignored COMBAT_ACTION, not the companion's turn\n");
                } else {
                    CoopCombatAction action;
                    memcpy(&action, payload, sizeof(action));
                    if (action.actionType == COOP_COMBAT_ACTION_MOVE) {
                        debug_printf("\nCoop: received COMBAT_ACTION move targetTile=%d\n", action.targetTile);
                        coopnet_host_apply_combat_move(action.targetTile);
                    } else if (action.actionType == COOP_COMBAT_ACTION_END_TURN) {
                        debug_printf("\nCoop: received COMBAT_ACTION end-turn\n");
                        g_coopHostCombatEndTurnRequested = true;
                    } else if (action.actionType == COOP_COMBAT_ACTION_ATTACK) {
                        debug_printf("\nCoop: received COMBAT_ACTION attack targetId=%d\n", action.targetId);
                        coopnet_host_apply_combat_attack(action.targetId);
                    }
                }
            }
        }

        uint32_t now = coopnet_now_ms();
        if (now - g_coopLastRecvTimeMs > kCoopHeartbeatTimeoutMs) {
            disconnected = true;
        }

        if (!disconnected) {
            coopnet_host_process_action_queue();
        }

        if (!disconnected && now - g_coopLastBroadcastTimeMs >= kCoopBroadcastIntervalMs) {
            coopnet_host_broadcast_positions();
            coopnet_host_broadcast_combat_participants();
            g_coopLastBroadcastTimeMs = now;
        }

        if (disconnected) {
            coopnet_close_socket(g_coopPeerSocket);
            g_coopRecvBufferLen = 0;
            g_coopConnState = CoopConnState::Listening;
        }
    }

    // Fallback follow-AI only while nobody is actively controlling the
    // companion over the network.
    if (g_coopConnState != CoopConnState::Connected) {
        coopnet_host_run_disconnected_follow();
    }
}

// Beyond this many tiles, animate a walk rather than instant-snapping is
// implausible (elevation change, reconnect, catch-up after a stall) — just
// snap. Within it, a real animated walk looks like natural movement instead
// of a teleport.
const int kCoopSnapDistanceThreshold = 8;

static void coopnet_client_apply_position(const CoopPosition& pos)
{
    Object* target = (pos.which == 0) ? g_coopCompanion : obj_dude;
    if (target == NULL) {
        return;
    }

    if (target->tile == pos.tile) {
        // Already there. obj_set_rotation() unconditionally reassigns
        // ->rotation and forces a redraw even when called with the value
        // it's already set to -- confirmed via testing that doing this on
        // every ~100ms broadcast, even while genuinely standing still,
        // produced a visible stutter/glitch on the client (constant
        // redraw fighting the object's own idle animation). Only touch it
        // when the rotation actually changed (e.g. turned in place).
        if (target->rotation != pos.rotation) {
            Rect rect;
            obj_set_rotation(target, pos.rotation, &rect);
            tile_refresh_rect(&rect, pos.elevation);
        }
        return;
    }

    if (target->elevation != pos.elevation || tile_dist(target->tile, pos.tile) > kCoopSnapDistanceThreshold) {
        Rect rect;
        obj_move_to_tile(target, pos.tile, pos.elevation, &rect);
        obj_set_rotation(target, pos.rotation, &rect);
        tile_refresh_rect(&rect, pos.elevation);
        g_coopLastCommandedTile[pos.which] = pos.tile;
        return;
    }

    if (g_coopLastCommandedTile[pos.which] == pos.tile) {
        return;
    }

    // Don't interrupt a run that's still in progress toward the previously
    // commanded tile — only issue a new command once the object has actually
    // arrived there (or this is the very first command for it). A fixed-time
    // throttle (reissue at most every N ms) was tried first, but the host
    // keeps walking, so the reported tile is almost always slightly
    // different every ~100ms broadcast regardless of the timer, and picking
    // an interval that didn't sometimes cut a stride short or leave a gap
    // was fragile — confirmed via testing it still produced an occasional
    // stop-start "crippled" gait. Checking real arrival is exact instead of
    // guessing a time window.
    bool hasArrived = g_coopLastCommandedTile[pos.which] == -1 || target->tile == g_coopLastCommandedTile[pos.which];
    if (!hasArrived) {
        return;
    }

    register_clear(target);
    register_begin(ANIMATION_REQUEST_UNRESERVED);
    register_object_run_to_tile(target, pos.tile, pos.elevation, -1, 0);
    register_end();
    g_coopLastCommandedTile[pos.which] = pos.tile;
}

// Client-side only: finds the local mirror object for a host participant id,
// or NULL if none is currently tracked (e.g. combat hasn't started, or that
// participant was never broadcast/already cleaned up).
static Object* coopnet_find_participant_object(int32_t hostId)
{
    for (int i = 0; i < g_coopParticipantCount; i++) {
        if (g_coopParticipants[i].hostId == hostId) {
            return g_coopParticipants[i].localObject;
        }
    }
    return NULL;
}

// Client-side only, the reverse of the above: given a local object (e.g.
// one the player just clicked on), finds the host participant id it mirrors,
// or -1 if it isn't a tracked participant at all (some other, unsynced NPC).
static int32_t coopnet_find_participant_id(Object* localObject)
{
    for (int i = 0; i < g_coopParticipantCount; i++) {
        if (g_coopParticipants[i].localObject == localObject) {
            return g_coopParticipants[i].hostId;
        }
    }
    return -1;
}

// How far a candidate local critter may be from a newly-reported
// participant's tile and still be adopted as its pre-existing mirror,
// rather than spawning a fresh duplicate -- see coopnet_apply_combat_participant()'s
// comment for why an exact tile match is too strict.
const int kCoopParticipantAdoptDistance = 20;

// Client-side only: applies one COOP_MSG_COMBAT_PARTICIPANT. First time seen
// for that id, finds-or-spawns a local mirror object (same pattern as
// coopnet_find_or_spawn_companion() -- prefer an object that already exists
// in the client's own independently-loaded map over spawning a fresh one).
// Matches by nearest unclaimed critter of the same pid within
// kCoopParticipantAdoptDistance tiles, not an exact tile match -- confirmed
// via testing that requiring an exact match regularly failed and produced
// visible duplicates: participant broadcasts share the same ~100ms cadence
// as combat processing, so by the time the first one for a given id reaches
// the client, the real enemy has often already taken a step or two away from
// its original map-spawn tile, meaning the exact-match search found nothing,
// spawned a brand new mirror, and left the real, never-adopted local critter
// wandering around too -- looking like "the client has more enemies than the
// host." Subsequent calls for an already-tracked id just update position/
// rotation/hp; a dead participant is removed from the map entirely (no
// attempt to sync the death animation).
static void coopnet_apply_combat_participant(const CoopCombatParticipant& p)
{
    Object* object = coopnet_find_participant_object(p.id);
    bool wasSpawned = false;

    if (object == NULL) {
        if (g_coopParticipantCount >= kCoopMaxParticipants) {
            debug_printf("\nCoop: participant table full, dropping id=%d\n", p.id);
            return;
        }

        Object* bestCandidate = NULL;
        int bestDist = kCoopParticipantAdoptDistance + 1;
        for (Object* candidate = obj_find_first_at(p.elevation); candidate != NULL; candidate = obj_find_next_at()) {
            if (candidate->pid != p.pid || FID_TYPE(candidate->fid) != OBJ_TYPE_CRITTER) {
                continue;
            }
            if (candidate == obj_dude || candidate == g_coopCompanion) {
                continue;
            }
            if (coopnet_find_participant_id(candidate) != -1) {
                continue;
            }
            int dist = tile_dist(candidate->tile, p.tile);
            if (dist < bestDist) {
                bestDist = dist;
                bestCandidate = candidate;
            }
        }
        object = bestCandidate;

        if (object == NULL) {
            if (obj_pid_new(&object, p.pid) == -1) {
                debug_printf("\nCoop: obj_pid_new failed applying combat participant (pid=%d)\n", p.pid);
                return;
            }
            // See coopnet_apply_item_dropped()'s comment on the identical
            // bug: obj_connect() doesn't remove the node obj_pid_new()
            // already created in floatingObjects, leaving this object
            // double-linked and vulnerable to a double-free on the next map
            // transition. obj_move_to_tile() reuses the existing node.
            Rect rect;
            obj_move_to_tile(object, p.tile, p.elevation, &rect);
            tile_refresh_rect(&rect, p.elevation);
            wasSpawned = true;
        }

        int index = g_coopParticipantCount++;
        g_coopParticipants[index].hostId = p.id;
        g_coopParticipants[index].localObject = object;
        g_coopParticipants[index].wasSpawned = wasSpawned;
        g_coopParticipants[index].lastCommandedTile = -1;
        debug_printf("\nCoop: tracking new combat participant id=%d pid=%d tile=%d spawned=%d\n", p.id, p.pid, p.tile, wasSpawned);
    }

    int index = -1;
    for (int i = 0; i < g_coopParticipantCount; i++) {
        if (g_coopParticipants[i].hostId == p.id) {
            index = i;
            break;
        }
    }
    if (index == -1) {
        return;
    }

    if (p.isDead) {
        debug_printf("\nCoop-debug: removing participant id=%d pid=%d (host reported isDead) hp=%d\n", p.id, p.pid, p.hp);
        obj_destroy(g_coopParticipants[index].localObject);
        g_coopParticipants[index] = g_coopParticipants[g_coopParticipantCount - 1];
        g_coopParticipantCount--;
        return;
    }

    // Same three-way split as coopnet_client_apply_position(): already
    // there (rotation only), a jump large enough that an animated run would
    // look implausible (snap instantly), or a normal step (smooth animated
    // run, throttled to real arrival so a new ~100ms update doesn't cut the
    // stride short -- see g_coopParticipants[].lastCommandedTile's comment).
    // Without this, participants were reported via instant obj_move_to_tile()
    // snaps only, which looked like teleporting on the client even though
    // the same movement looked like normal animated walking on the host
    // (confirmed via testing).
    if (object->tile == p.tile) {
        if (object->rotation != p.rotation) {
            Rect rect;
            obj_set_rotation(object, p.rotation, &rect);
            tile_refresh_rect(&rect, p.elevation);
        }
        return;
    }

    if (object->elevation != p.elevation || tile_dist(object->tile, p.tile) > kCoopSnapDistanceThreshold) {
        Rect rect;
        obj_move_to_tile(object, p.tile, p.elevation, &rect);
        obj_set_rotation(object, p.rotation, &rect);
        tile_refresh_rect(&rect, p.elevation);
        g_coopParticipants[index].lastCommandedTile = p.tile;
        return;
    }

    if (g_coopParticipants[index].lastCommandedTile == p.tile) {
        return;
    }

    bool hasArrived = g_coopParticipants[index].lastCommandedTile == -1 || object->tile == g_coopParticipants[index].lastCommandedTile;
    if (!hasArrived) {
        return;
    }

    register_clear(object);
    register_begin(ANIMATION_REQUEST_UNRESERVED);
    register_object_run_to_tile(object, p.tile, p.elevation, -1, 0);
    register_end();
    g_coopParticipants[index].lastCommandedTile = p.tile;
}

// Client-side only: called on COOP_MSG_COMBAT_END. Destroys only the
// participant mirrors that were freshly spawned for this fight (an adopted
// one is a real, persistent map object and is left alone) and clears the
// table -- host-side ids are only meaningful for the combat that assigned
// them.
static void coopnet_clear_combat_participants()
{
    for (int i = 0; i < g_coopParticipantCount; i++) {
        if (g_coopParticipants[i].wasSpawned) {
            obj_destroy(g_coopParticipants[i].localObject);
        }
    }
    g_coopParticipantCount = 0;
}

// Client-side only: applies a COOP_MSG_MAP_TRANSITION. The host is the sole
// authority on when a transition happens (see object.cc's obj_move_to_tile()
// client-role guard and coopnet_host_check_map_transition()'s comment) -- this
// force-loads the same map file locally and places the mirrored obj_dude and
// a fresh local companion placeholder at the given spot. Exact companion
// position doesn't matter much here since the next COOP_MSG_POSITION
// broadcast (moments away) corrects it.
static void coopnet_client_apply_map_transition(const CoopMapTransition& transition)
{
    char mapName[16];
    strncpy(mapName, transition.mapName, sizeof(mapName) - 1);
    mapName[sizeof(mapName) - 1] = '\0';

    debug_printf("\nCoop: client applying MAP_TRANSITION to %s (tile=%d elevation=%d)\n", mapName, transition.tile, transition.elevation);

    g_coopClientInCombat = false;
    g_coopClientCombatTurnActive = false;
    coopnet_clear_combat_participants();

    // This bypasses map_leave_map()/map_check_state() entirely (it's not a
    // real in-engine exit trigger, just a direct by-name load), so
    // map_data.cc's own file-static map_state is never populated for this
    // call -- reset it first so map_load_file()'s tail code doesn't read
    // stale/garbage data. See map_reset_transition_state()'s comment.
    map_reset_transition_state();

    if (map_load(mapName) == -1) {
        debug_printf("\nCoop: client failed to load map %s for MAP_TRANSITION\n", mapName);
        return;
    }

    // map_load() alone leaves the background sound channel on "wind2" (a
    // generic loading/transition sound played inside map_load_file()) --
    // vanilla always follows it with PlayCityMapMusic() to switch to the
    // actual location's real music/ambience, but only from map_load_idx(),
    // which this function deliberately bypasses (it needs to load a map by
    // *name*, from the host, not by map index). Without this, the client's
    // background channel got stuck on the wind sound after every single
    // transition -- confirmed via testing.
    PlayCityMapMusic();

    if (hexGridTileIsValid(transition.tile) && elevationIsValid(transition.elevation)) {
        obj_move_to_tile(obj_dude, transition.tile, transition.elevation, NULL);
        map_set_elevation(transition.elevation);
        obj_set_rotation(obj_dude, transition.rotation, NULL);
    }

    if (tile_set_center(obj_dude->tile, TILE_SET_CENTER_REFRESH_WINDOW) == -1) {
        debug_printf("\nCoop: client attempt to center out-of-bounds after MAP_TRANSITION\n");
    }

    g_coopCompanion = coopnet_respawn_companion(g_coopCompanion, obj_dude->pid, obj_dude->tile, obj_dude->elevation);

    g_coopLastCommandedTile[0] = -1;
    g_coopLastCommandedTile[1] = -1;
}

// Client-side only: a small, always-on-top, non-modal window mirroring the
// host's current dialogue reply + option list, overlaid on the client's own
// normal game view (the client keeps playing/watching the world as usual --
// it never switches to a dedicated dialogue screen like the host does).
// Recreated from scratch on every COOP_MSG_DIALOGUE_STATE (simplest way to
// size it correctly for however much text there is this time); -1 when no
// conversation is being mirrored. This is a plain bordered text box, not a
// pixel replica of the host's graphical dialogue window (portrait,
// background art, button layout) -- good enough for the client to read
// along and see the option list without the much larger effort of
// recreating that real UI in a read-only "puppet" mode.
static int g_coopDialogueWin = -1;

const int kCoopDialogueWinX = 10;
const int kCoopDialogueWinY = 10;
const int kCoopDialogueWinWidth = 460;
const int kCoopDialogueWinHeight = 280;
const int kCoopDialogueWinPadding = 10;

static void coopnet_close_dialogue_window()
{
    if (g_coopDialogueWin != -1) {
        win_delete(g_coopDialogueWin);
        g_coopDialogueWin = -1;
    }
}

// Word-wraps `text` to fit within `maxWidth` pixels, drawing each resulting
// line into `buf` (a window's own pixel buffer, `bufWidth` wide) starting at
// (`x`, `*y`), advancing `*y` by text_height() per line drawn -- same
// buffer-offset convention win_msg() uses (buf + bufWidth * y + x).
static void coopnet_dialogue_draw_wrapped(unsigned char* buf, int bufWidth, int maxWidth, int x, int* y, const char* text, int color)
{
    char line[256] = "";
    char word[128] = "";
    int wordLen = 0;

    for (const char* p = text;; p++) {
        char c = *p;
        if (c == ' ' || c == '\0') {
            word[wordLen] = '\0';

            if (wordLen > 0) {
                char candidate[256];
                if (line[0] != '\0') {
                    snprintf(candidate, sizeof(candidate), "%s %s", line, word);
                } else {
                    snprintf(candidate, sizeof(candidate), "%s", word);
                }

                if (line[0] != '\0' && text_width(candidate) > maxWidth) {
                    text_to_buf(buf + bufWidth * (*y) + x, line, bufWidth, bufWidth, color);
                    *y += text_height();
                    snprintf(line, sizeof(line), "%s", word);
                } else {
                    strncpy(line, candidate, sizeof(line) - 1);
                    line[sizeof(line) - 1] = '\0';
                }
            }

            wordLen = 0;
            if (c == '\0') {
                break;
            }
        } else if (wordLen < static_cast<int>(sizeof(word)) - 1) {
            word[wordLen++] = c;
        }
    }

    if (line[0] != '\0') {
        text_to_buf(buf + bufWidth * (*y) + x, line, bufWidth, bufWidth, color);
        *y += text_height();
    }
}

// Client-side only: (re)draws the dialogue-mirror window with the current
// reply text + option list, creating it first if this is the first state of
// a new conversation.
static void coopnet_client_apply_dialogue_state(const CoopDialogueState& state)
{
    coopnet_close_dialogue_window();

    g_coopDialogueWin = win_add(kCoopDialogueWinX, kCoopDialogueWinY, kCoopDialogueWinWidth, kCoopDialogueWinHeight, 256, WINDOW_MOVE_ON_TOP);
    if (g_coopDialogueWin == -1) {
        return;
    }

    win_border(g_coopDialogueWin);

    Window* window = GNW_find(g_coopDialogueWin);
    unsigned char* buf = window->buffer;
    int textWidth = kCoopDialogueWinWidth - 2 * kCoopDialogueWinPadding;
    int y = kCoopDialogueWinPadding;
    int textColor = colorTable[GNW_wcolor[3]];

    if (state.replyText[0] != '\0') {
        coopnet_dialogue_draw_wrapped(buf, kCoopDialogueWinWidth, textWidth, kCoopDialogueWinPadding, &y, state.replyText, textColor);
        y += text_height() / 2;
    }

    int count = state.optionCount;
    if (count > kCoopMaxDialogueOptions) {
        count = kCoopMaxDialogueOptions;
    }
    for (int i = 0; i < count; i++) {
        if (state.optionText[i][0] == '\0') {
            continue;
        }

        char labeled[256];
        snprintf(labeled, sizeof(labeled), "%d. %s", i + 1, state.optionText[i]);
        coopnet_dialogue_draw_wrapped(buf, kCoopDialogueWinWidth, textWidth, kCoopDialogueWinPadding, &y, labeled, textColor);
    }

    win_draw(g_coopDialogueWin);
}

static void coopnet_poll_client()
{
    coopnet_protect_companion_item_scripts();

    if (g_coopConnState == CoopConnState::Connecting) {
        fd_set writeSet;
        FD_ZERO(&writeSet);
        FD_SET(g_coopPeerSocket, &writeSet);

        timeval timeout;
        timeout.tv_sec = 0;
        timeout.tv_usec = 0;

        fd_set exceptSet;
        FD_ZERO(&exceptSet);
        FD_SET(g_coopPeerSocket, &exceptSet);

        int rc = select(static_cast<int>(g_coopPeerSocket) + 1, NULL, &writeSet, &exceptSet, &timeout);
        if (rc > 0 && FD_ISSET(g_coopPeerSocket, &exceptSet)) {
            debug_printf("\nCoop: connect() failed (exception on socket)\n");
            coopnet_shutdown();
        } else if (rc > 0 && FD_ISSET(g_coopPeerSocket, &writeSet)) {
            int soError = 0;
#ifdef _WIN32
            int soErrorLen = sizeof(soError);
#else
            socklen_t soErrorLen = sizeof(soError);
#endif
            getsockopt(g_coopPeerSocket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soError), &soErrorLen);
            if (soError != 0) {
                debug_printf("\nCoop: connect() failed (SO_ERROR=%d)\n", soError);
                coopnet_shutdown();
            } else {
                debug_printf("\nCoop: TCP connect established, sending HELLO\n");
                CoopHello hello;
                hello.protocolVersion = kCoopProtocolVersion;
                memset(hello.mapName, 0, sizeof(hello.mapName));
                strncpy(hello.mapName, map_data.name, sizeof(hello.mapName) - 1);
                bool sent = coopnet_send_message(g_coopPeerSocket, COOP_MSG_HELLO, &hello, sizeof(hello));
                debug_printf("\nCoop: HELLO sent, success=%d\n", sent);
                g_coopConnState = CoopConnState::WaitingForAck;
                g_coopLastRecvTimeMs = coopnet_now_ms();
            }
        }
    }

    if (g_coopConnState == CoopConnState::WaitingForAck) {
        uint8_t type;
        unsigned char payload[kCoopMaxMessagePayload];
        uint16_t payloadLen;
        if (coopnet_try_recv_message(g_coopPeerSocket, &type, payload, &payloadLen)) {
            debug_printf("\nCoop: client received message type=%d len=%d while waiting for HELLO_ACK\n", type, payloadLen);
            if (type == COOP_MSG_HELLO_ACK && payloadLen == sizeof(CoopHelloAck)) {
                CoopHelloAck ack;
                memcpy(&ack, payload, sizeof(ack));
                debug_printf("\nCoop: HELLO_ACK received (accepted=%d, companionPid=%d, companionTile=%d)\n", ack.accepted, ack.companionPid, ack.companionTile);
                if (ack.accepted != 0) {
                    g_coopCompanion = coopnet_find_or_spawn_companion(ack.companionPid, ack.companionTile, ack.companionElevation);
                    g_coopConnState = CoopConnState::Connected;
                    g_coopLastRecvTimeMs = coopnet_now_ms();
                    debug_printf("\nCoop: now Connected (companion=%p)\n", (void*)g_coopCompanion);

                    // One-time instant camera snap onto the companion, the
                    // character the client is meant to be playing as. After
                    // this, normal camera controls (mouse-edge scroll, Home
                    // key) take over — vanilla gameplay doesn't auto-scroll
                    // on every step either, so neither does this.
                    if (g_coopCompanion != NULL) {
                        tile_set_center(g_coopCompanion->tile, TILE_SET_CENTER_REFRESH_WINDOW | TILE_SET_CENTER_FLAG_IGNORE_SCROLL_RESTRICTIONS);
                    }

                    win_msg("Host connected!", 100, 100, 0);
                }
            }
        }
    }

    if (g_coopConnState == CoopConnState::Connected) {
        uint8_t type;
        unsigned char payload[kCoopMaxMessagePayload];
        uint16_t payloadLen;
        bool disconnected = false;

        // If this window was in the background for a while, many queued
        // position updates can arrive in one burst. Applying every one of
        // them back-to-back (each an instant snap, not a smooth walk) can
        // leave stale sprite copies on screen ("ghosting"/"multiplying"
        // characters, confirmed via testing) before the next real redraw.
        // Only the most recent position per object actually matters, so
        // drain the whole backlog but apply just the latest of each.
        bool havePos[2] = { false, false };
        CoopPosition latestPos[2];

        while (coopnet_try_recv_message(g_coopPeerSocket, &type, payload, &payloadLen)) {
            g_coopLastRecvTimeMs = coopnet_now_ms();
            if (type == COOP_MSG_POSITION && payloadLen == sizeof(CoopPosition)) {
                CoopPosition pos;
                memcpy(&pos, payload, sizeof(pos));
                if (pos.which < 2) {
                    latestPos[pos.which] = pos;
                    havePos[pos.which] = true;
                }
            } else if (type == COOP_MSG_ITEM_DROPPED && payloadLen == sizeof(CoopItemEvent)) {
                CoopItemEvent evt;
                memcpy(&evt, payload, sizeof(evt));
                debug_printf("\nCoop: received ITEM_DROPPED from host (pid=%d, tile=%d)\n", evt.pid, evt.tile);
                coopnet_apply_item_dropped(evt);
            } else if (type == COOP_MSG_ITEM_PICKED_UP && payloadLen == sizeof(CoopItemEvent)) {
                CoopItemEvent evt;
                memcpy(&evt, payload, sizeof(evt));
                debug_printf("\nCoop: received ITEM_PICKED_UP from host (pid=%d, tile=%d)\n", evt.pid, evt.tile);
                coopnet_apply_item_picked_up(evt);
            } else if (type == COOP_MSG_COMPANION_INVENTORY && payloadLen == sizeof(CoopInventorySync) && kCoopCompanionInventorySyncEnabled) {
                CoopInventorySync sync;
                memcpy(&sync, payload, sizeof(sync));
                debug_printf("\nCoop: received COMPANION_INVENTORY from host (%d items)\n", sync.itemCount);
                coopnet_apply_companion_inventory(sync);
            } else if (type == COOP_MSG_COMBAT_TURN && payloadLen == sizeof(CoopCombatTurn)) {
                CoopCombatTurn turn;
                memcpy(&turn, payload, sizeof(turn));
                g_coopClientCombatTurnActive = turn.actionPoints > 0;
                debug_printf("\nCoop: received COMBAT_TURN ap=%d, myTurnActive=%d\n", turn.actionPoints, g_coopClientCombatTurnActive);
            } else if (type == COOP_MSG_COMBAT_BEGIN) {
                debug_printf("\nCoop: received COMBAT_BEGIN\n");
                // Defensive: guarantees a clean slate even if a previous
                // fight's COMBAT_END was somehow missed (a dropped/delayed
                // message, or a disconnect mid-fight) -- without this, any
                // participant left over from that earlier fight would keep
                // being treated as "already tracked" and never get its
                // position corrected, since a brand new fight hands out
                // fresh ids that won't match the stale entries.
                coopnet_clear_combat_participants();
                g_coopClientInCombat = true;
                win_msg("Combat has started!", 100, 100, 0);
            } else if (type == COOP_MSG_COMBAT_END) {
                debug_printf("\nCoop: received COMBAT_END\n");
                g_coopClientInCombat = false;
                g_coopClientCombatTurnActive = false;
                coopnet_clear_combat_participants();
                win_msg("Combat has ended.", 100, 100, 0);
            } else if (type == COOP_MSG_COMBAT_PARTICIPANT && payloadLen == sizeof(CoopCombatParticipant)) {
                CoopCombatParticipant participant;
                memcpy(&participant, payload, sizeof(participant));
                coopnet_apply_combat_participant(participant);
            } else if (type == COOP_MSG_MAP_TRANSITION && payloadLen == sizeof(CoopMapTransition)) {
                CoopMapTransition transition;
                memcpy(&transition, payload, sizeof(transition));
                coopnet_client_apply_map_transition(transition);
            } else if (type == COOP_MSG_DIALOGUE_BEGIN) {
                char buffer[64];
                strcpy(buffer, "The host has started a conversation.");
                display_print(buffer);
            } else if (type == COOP_MSG_DIALOGUE_STATE && payloadLen == sizeof(CoopDialogueState)) {
                CoopDialogueState state;
                memcpy(&state, payload, sizeof(state));
                coopnet_client_apply_dialogue_state(state);
            } else if (type == COOP_MSG_DIALOGUE_END) {
                char buffer[64];
                strcpy(buffer, "The conversation has ended.");
                display_print(buffer);
                coopnet_close_dialogue_window();
            } else if (type == COOP_MSG_GAME_OVER && payloadLen == sizeof(CoopGameOver)) {
                CoopGameOver gameOver;
                memcpy(&gameOver, payload, sizeof(gameOver));
                coopnet_close_dialogue_window();

                debug_printf("\nCoop: received GAME_OVER (reason=%d)\n", gameOver.reason);

                // Same real death cutscene/narration obj_dude's own death
                // triggers on the host -- requested directly: both players
                // should see the same "game over" screen either way, not a
                // plain text box. See main_request_death_scene()'s comment
                // in main.h for why game_user_wants_to_quit must be exactly
                // 2, not merely nonzero.
                main_request_death_scene();
                game_user_wants_to_quit = 2;
            }
        }

        for (int which = 0; which < 2; which++) {
            if (havePos[which]) {
                debug_printf("\nCoop: applying POSITION which=%d tile=%d elevation=%d\n", latestPos[which].which, latestPos[which].tile, latestPos[which].elevation);
                coopnet_client_apply_position(latestPos[which]);
            }
        }

        uint32_t now = coopnet_now_ms();
        if (now - g_coopLastRecvTimeMs > kCoopHeartbeatTimeoutMs) {
            disconnected = true;
        }

        // The client otherwise only sends anything when the player clicks to
        // move the companion — without this, the host's own heartbeat
        // timeout fires after a few idle seconds and wrongly closes a
        // perfectly healthy connection (confirmed via testing).
        if (!disconnected && now - g_coopLastHeartbeatSentTimeMs >= kCoopHeartbeatIntervalMs) {
            coopnet_send_message(g_coopPeerSocket, COOP_MSG_HEARTBEAT, NULL, 0);
            g_coopLastHeartbeatSentTimeMs = now;
        }

        if (disconnected) {
            coopnet_shutdown();
        }
    }
}

void coopnet_poll()
{
    switch (g_coopRole) {
    case CoopRole::Host:
        coopnet_poll_host();
        break;
    case CoopRole::Client:
        coopnet_poll_client();
        break;
    case CoopRole::None:
        break;
    }
}

// Host-side only, called from combat.cc's combat_turn() when a1 is the
// companion and a client is connected: takes over the companion's combat
// turn, driven over the network instead of local AI (combat_ai()) or local
// player input (combat_input(), the obj_dude case). Mirrors combat_input()'s
// shape -- a blocking loop pumping input until the turn ends -- but the
// "input" here is COOP_MSG_COMBAT_ACTION messages drained by coopnet_poll()
// instead of the keyboard/mouse. Movement only for now (milestone 3 plan,
// phase 2) -- attacks are a later phase.
void coopnet_combat_input(Object* companion)
{
    if (g_coopRole != CoopRole::Host) {
        // Confirmed via testing: this must never run on the client. NPCs
        // run independently/unsynced on each side (see the general
        // "simulation divergence" limitation), so it's entirely possible
        // for the client's own local, independent combat simulation to
        // also reach the companion's turn in its own combat_list[] around
        // the same time as the host's real combat. Without this guard,
        // combat_turn()'s dispatch (see combat.cc) would call this on the
        // client too, running a second, spurious copy of this blocking
        // network loop -- the likely cause of a crash seen during testing
        // right as the companion's turn began.
        return;
    }

    if (!coopnet_is_connected()) {
        return;
    }

    CoopCombatTurn turn;
    turn.actionPoints = companion->data.critter.combat.ap;
    coopnet_send_message(g_coopPeerSocket, COOP_MSG_COMBAT_TURN, &turn, sizeof(turn));
    debug_printf("\nCoop: companion combat turn started, ap=%d\n", turn.actionPoints);

    g_coopHostCombatTurnActive = true;
    g_coopHostCombatEndTurnRequested = false;

    while (true) {
        sharedFpsLimiter.mark();

        coopnet_poll();
        process_bk();

        if (companion->data.critter.combat.ap <= 0) {
            break;
        }
        if (g_coopHostCombatEndTurnRequested) {
            break;
        }
        if (!coopnet_is_connected()) {
            break;
        }
        if (game_user_wants_to_quit != 0) {
            break;
        }

        renderPresent();
        sharedFpsLimiter.throttle();
    }

    g_coopHostCombatTurnActive = false;

    if (coopnet_is_connected()) {
        CoopCombatTurn endTurn;
        endTurn.actionPoints = 0;
        coopnet_send_message(g_coopPeerSocket, COOP_MSG_COMBAT_TURN, &endTurn, sizeof(endTurn));
        debug_printf("\nCoop: companion combat turn ended\n");
    }
}

void coopnet_on_client_click(int tile)
{
    if (g_coopConnState != CoopConnState::Connected) {
        debug_printf("\nCoop: click on tile=%d ignored, not connected (state=%d)\n", tile, static_cast<int>(g_coopConnState));
        return;
    }

    CoopMoveIntent intent;
    intent.targetTile = tile;
    bool sent = coopnet_send_message(g_coopPeerSocket, COOP_MSG_MOVE_INTENT, &intent, sizeof(intent));
    debug_printf("\nCoop: sent MOVE_INTENT targetTile=%d success=%d\n", tile, sent);
}

bool coopnet_is_companion_turn_active()
{
    return g_coopClientCombatTurnActive;
}

void coopnet_on_client_combat_move_click(int tile)
{
    if (g_coopConnState != CoopConnState::Connected) {
        return;
    }

    CoopCombatAction action;
    action.actionType = COOP_COMBAT_ACTION_MOVE;
    action.targetTile = tile;
    action.targetId = -1;
    bool sent = coopnet_send_message(g_coopPeerSocket, COOP_MSG_COMBAT_ACTION, &action, sizeof(action));
    debug_printf("\nCoop: sent COMBAT_ACTION move targetTile=%d success=%d\n", tile, sent);
}

void coopnet_on_client_end_turn()
{
    if (g_coopConnState != CoopConnState::Connected) {
        return;
    }

    CoopCombatAction action;
    action.actionType = COOP_COMBAT_ACTION_END_TURN;
    action.targetTile = -1;
    action.targetId = -1;
    bool sent = coopnet_send_message(g_coopPeerSocket, COOP_MSG_COMBAT_ACTION, &action, sizeof(action));
    debug_printf("\nCoop: sent COMBAT_ACTION end-turn success=%d\n", sent);
}

void coopnet_on_client_attack(Object* target)
{
    if (g_coopConnState != CoopConnState::Connected) {
        return;
    }

    int32_t targetId = (target != NULL) ? coopnet_find_participant_id(target) : -1;

    CoopCombatAction action;
    action.actionType = COOP_COMBAT_ACTION_ATTACK;
    action.targetTile = -1;
    action.targetId = targetId;
    bool sent = coopnet_send_message(g_coopPeerSocket, COOP_MSG_COMBAT_ACTION, &action, sizeof(action));
    debug_printf("\nCoop: sent COMBAT_ACTION attack targetId=%d success=%d\n", targetId, sent);
}

void coopnet_on_client_pickup_click(int pid, int tile, int elevation)
{
    if (g_coopConnState != CoopConnState::Connected) {
        debug_printf("\nCoop: pickup click on pid=%d tile=%d ignored, not connected (state=%d)\n", pid, tile, static_cast<int>(g_coopConnState));
        return;
    }

    CoopItemEvent evt;
    evt.pid = pid;
    evt.tile = tile;
    evt.elevation = elevation;
    bool sent = coopnet_send_message(g_coopPeerSocket, COOP_MSG_PICKUP_REQUEST, &evt, sizeof(evt));
    debug_printf("\nCoop: sent PICKUP_REQUEST pid=%d tile=%d success=%d\n", pid, tile, sent);
}

void coopnet_on_client_use_click(int pid, int tile, int elevation)
{
    if (g_coopConnState != CoopConnState::Connected) {
        debug_printf("\nCoop: use click on pid=%d tile=%d ignored, not connected (state=%d)\n", pid, tile, static_cast<int>(g_coopConnState));
        return;
    }

    CoopItemEvent evt;
    evt.pid = pid;
    evt.tile = tile;
    evt.elevation = elevation;
    bool sent = coopnet_send_message(g_coopPeerSocket, COOP_MSG_USE_REQUEST, &evt, sizeof(evt));
    debug_printf("\nCoop: sent USE_REQUEST pid=%d tile=%d success=%d\n", pid, tile, sent);
}

void coopnet_on_client_skill_use(int skill, Object* target)
{
    if (target == NULL) {
        return;
    }

    if (g_coopConnState != CoopConnState::Connected) {
        debug_printf("\nCoop: skill click on pid=%d tile=%d ignored, not connected (state=%d)\n", target->pid, target->tile, static_cast<int>(g_coopConnState));
        return;
    }

    CoopSkillRequest req;
    req.skill = skill;
    req.pid = target->pid;
    req.tile = target->tile;
    req.elevation = target->elevation;
    req.targetIsHostDude = (target == obj_dude) ? 1 : 0;
    bool sent = coopnet_send_message(g_coopPeerSocket, COOP_MSG_SKILL_REQUEST, &req, sizeof(req));
    debug_printf("\nCoop: sent SKILL_REQUEST skill=%d pid=%d tile=%d targetIsHostDude=%d success=%d\n", skill, target->pid, target->tile, req.targetIsHostDude, sent);
}

void coopnet_notify_item_dropped(Object* critter, Object* item)
{
    if (g_coopConnState != CoopConnState::Connected) {
        return;
    }
    if (item == NULL || (critter != obj_dude && critter != g_coopCompanion)) {
        return;
    }

    // item->tile/elevation already reflect where it landed — obj_drop()
    // calls obj_connect() before we're invoked (see protinst.cc).
    CoopItemEvent evt;
    evt.pid = item->pid;
    evt.tile = item->tile;
    evt.elevation = item->elevation;
    bool sent = coopnet_send_message(g_coopPeerSocket, COOP_MSG_ITEM_DROPPED, &evt, sizeof(evt));
    debug_printf("\nCoop: notified peer of item drop (pid=%d, tile=%d) success=%d\n", evt.pid, evt.tile, sent);
}

void coopnet_notify_item_picked_up(Object* critter, Object* item)
{
    if (g_coopConnState != CoopConnState::Connected) {
        return;
    }
    if (item == NULL || (critter != obj_dude && critter != g_coopCompanion)) {
        return;
    }

    if (critter == g_coopCompanion) {
        // Lets coopnet_host_process_action_queue() dispatch the next queued
        // request right away instead of waiting out the safety timeout.
        g_coopCompanionActionBusy = false;

        if (g_coopRole == CoopRole::Host && kCoopCompanionInventorySyncEnabled) {
            // item_add_mult() already ran by this point in obj_pickup(), so
            // the companion's inventory already reflects this pickup.
            coopnet_host_broadcast_companion_inventory();
        }
    }

    // Must be called with item->tile/elevation still valid, i.e. before
    // obj_pickup() calls obj_disconnect() (see protinst.cc).
    CoopItemEvent evt;
    evt.pid = item->pid;
    evt.tile = item->tile;
    evt.elevation = item->elevation;
    bool sent = coopnet_send_message(g_coopPeerSocket, COOP_MSG_ITEM_PICKED_UP, &evt, sizeof(evt));
    debug_printf("\nCoop: notified peer of item pickup (pid=%d, tile=%d) success=%d\n", evt.pid, evt.tile, sent);
}

void coopnet_notify_combat_begin()
{
    if (g_coopRole != CoopRole::Host || g_coopConnState != CoopConnState::Connected) {
        return;
    }

    bool sent = coopnet_send_message(g_coopPeerSocket, COOP_MSG_COMBAT_BEGIN, NULL, 0);
    debug_printf("\nCoop: notified peer combat began, success=%d\n", sent);
}

void coopnet_notify_combat_end()
{
    if (g_coopRole != CoopRole::Host || g_coopConnState != CoopConnState::Connected) {
        return;
    }

    bool sent = coopnet_send_message(g_coopPeerSocket, COOP_MSG_COMBAT_END, NULL, 0);
    debug_printf("\nCoop: notified peer combat ended, success=%d\n", sent);
}

void coopnet_notify_dialogue_begin()
{
    if (g_coopRole != CoopRole::Host || g_coopConnState != CoopConnState::Connected) {
        return;
    }

    bool sent = coopnet_send_message(g_coopPeerSocket, COOP_MSG_DIALOGUE_BEGIN, NULL, 0);
    debug_printf("\nCoop: notified peer dialogue began, success=%d\n", sent);
}

void coopnet_notify_dialogue_end()
{
    if (g_coopRole != CoopRole::Host || g_coopConnState != CoopConnState::Connected) {
        return;
    }

    bool sent = coopnet_send_message(g_coopPeerSocket, COOP_MSG_DIALOGUE_END, NULL, 0);
    debug_printf("\nCoop: notified peer dialogue ended, success=%d\n", sent);
}

void coopnet_notify_dialogue_state(const char* replyText, const char* const* optionTexts, int optionCount)
{
    if (g_coopRole != CoopRole::Host || g_coopConnState != CoopConnState::Connected) {
        return;
    }

    CoopDialogueState state;
    memset(&state, 0, sizeof(state));
    strncpy(state.replyText, replyText != NULL ? replyText : "", sizeof(state.replyText) - 1);

    int count = optionCount;
    if (count > kCoopMaxDialogueOptions) {
        count = kCoopMaxDialogueOptions;
    }
    if (count < 0) {
        count = 0;
    }
    state.optionCount = static_cast<uint8_t>(count);
    for (int i = 0; i < count; i++) {
        strncpy(state.optionText[i], optionTexts[i] != NULL ? optionTexts[i] : "", sizeof(state.optionText[i]) - 1);
    }

    bool sent = coopnet_send_message(g_coopPeerSocket, COOP_MSG_DIALOGUE_STATE, &state, sizeof(state));
    debug_printf("\nCoop: notified peer dialogue state (reply=\"%.40s\" options=%d) success=%d\n", state.replyText, count, sent);
}

void coopnet_notify_game_over(uint8_t reason)
{
    if (g_coopRole != CoopRole::Host || g_coopConnState != CoopConnState::Connected) {
        return;
    }

    CoopGameOver gameOver;
    gameOver.reason = reason;

    bool sent = coopnet_send_message(g_coopPeerSocket, COOP_MSG_GAME_OVER, &gameOver, sizeof(gameOver));
    debug_printf("\nCoop: notified peer game over (reason=%d) success=%d\n", reason, sent);
}

} // namespace fallout
