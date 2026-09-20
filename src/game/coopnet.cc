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

#include "game/anim.h"
#include "game/map.h"
#include "game/object.h"
#include "game/party.h"
#include "game/protinst.h"
#include "game/tile.h"
#include "plib/gnw/debug.h"
#include "plib/gnw/intrface.h"
#include "plib/gnw/rect.h"

namespace fallout {

// ---------------------------------------------------------------------------
// Wire protocol (milestone 1: LAN connect + watch each other move)
// ---------------------------------------------------------------------------

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

// Used for both COOP_MSG_ITEM_DROPPED and COOP_MSG_ITEM_PICKED_UP. Identifies
// an item by (pid, tile, elevation) rather than a shared unique id — see the
// caveat on coopnet_notify_item_dropped()/coopnet_notify_item_picked_up() in
// coopnet.h.
struct CoopItemEvent {
    int32_t pid;
    int32_t tile;
    int32_t elevation;
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

// NOTE: deliberately very generous for now while the engine's background-focus
// behavior (see GNW95_lost_focus) is still being made fully reliable during
// coop sessions. Tighten this back down once that's solid.
const uint32_t kCoopHeartbeatTimeoutMs = 120000;
const uint32_t kCoopHeartbeatIntervalMs = 2000;
const uint32_t kCoopBroadcastIntervalMs = 100;
const uint32_t kCoopFollowCheckIntervalMs = 1500;
const int kCoopFollowDistanceThreshold = 3;

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

    unsigned char buf[sizeof(CoopMsgHeader) + 256];
    CoopMsgHeader header;
    header.length = payloadLen;
    header.type = type;

    memcpy(buf, &header, sizeof(header));
    if (payloadLen > 0) {
        memcpy(buf + sizeof(header), payload, payloadLen);
    }

    int total = sizeof(header) + payloadLen;
    int sent = 0;
    while (sent < total) {
        int rc = send(sock, reinterpret_cast<const char*>(buf) + sent, total - sent, 0);
        if (rc <= 0) {
            if (coopnet_would_block()) {
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
    companion->flags |= OBJECT_NO_REMOVE;

    debug_printf("\nCoop: spawned new companion object (pid=%d, tile=%d)\n", pid, tile);

    return companion;
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

static void coopnet_apply_item_dropped(const CoopItemEvent& evt)
{
    Object* item = NULL;
    if (obj_pid_new(&item, evt.pid) == -1) {
        debug_printf("\nCoop: obj_pid_new failed applying item drop (pid=%d)\n", evt.pid);
        return;
    }

    Rect rect;
    obj_connect(item, evt.tile, evt.elevation, &rect);
    tile_refresh_rect(&rect, evt.elevation);
}

static void coopnet_apply_item_picked_up(const CoopItemEvent& evt)
{
    for (Object* object = obj_find_first_at(evt.elevation); object != NULL; object = obj_find_next_at()) {
        if (object->tile == evt.tile && object->pid == evt.pid) {
            obj_destroy(object);
            return;
        }
    }

    debug_printf("\nCoop: item pickup notification had no matching ground item (pid=%d, tile=%d)\n", evt.pid, evt.tile);
}

static void coopnet_poll_host()
{
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
        unsigned char payload[256];
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

                win_msg("Client connected!", 100, 100, 0);
            }
        }
    }

    if (g_coopConnState == CoopConnState::Connected) {
        bool disconnected = false;

        uint8_t type;
        unsigned char payload[256];
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
            }
        }

        uint32_t now = coopnet_now_ms();
        if (now - g_coopLastRecvTimeMs > kCoopHeartbeatTimeoutMs) {
            disconnected = true;
        }

        if (!disconnected && now - g_coopLastBroadcastTimeMs >= kCoopBroadcastIntervalMs) {
            coopnet_host_broadcast_positions();
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
        // Already there — only rotation may have changed (e.g. turned in place).
        Rect rect;
        obj_set_rotation(target, pos.rotation, &rect);
        tile_refresh_rect(&rect, pos.elevation);
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

static void coopnet_poll_client()
{
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
        unsigned char payload[256];
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
        unsigned char payload[256];
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

    // Must be called with item->tile/elevation still valid, i.e. before
    // obj_pickup() calls obj_disconnect() (see protinst.cc).
    CoopItemEvent evt;
    evt.pid = item->pid;
    evt.tile = item->tile;
    evt.elevation = item->elevation;
    bool sent = coopnet_send_message(g_coopPeerSocket, COOP_MSG_ITEM_PICKED_UP, &evt, sizeof(evt));
    debug_printf("\nCoop: notified peer of item pickup (pid=%d, tile=%d) success=%d\n", evt.pid, evt.tile, sent);
}

} // namespace fallout
