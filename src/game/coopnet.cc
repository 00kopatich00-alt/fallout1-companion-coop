#include "game/coopnet.h"

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <vector>

#include <SDL.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#pragma comment(lib, "iphlpapi.lib")
typedef SOCKET CoopSocket;
#define COOP_INVALID_SOCKET INVALID_SOCKET
#else
#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int CoopSocket;
#define COOP_INVALID_SOCKET (-1)
#endif

#include "game/actions.h"
#include "game/anim.h"
#include "game/art.h"
#include "game/cache.h"
#include "game/combat.h"
#include "game/combatai.h"
#include "game/config.h"
#include "game/critter.h"
#include "game/cycle.h"
#include "game/display.h"
#include "game/gdialog.h"
#include "game/gmouse.h"
#include "game/gsound.h"
#include "game/heap.h"
#include "game/game.h"
#include "game/gconfig.h"
#include "platform_compat.h"
#include "game/intface.h"
#include "game/item.h"
#include "game/main.h"
#include "game/map.h"
#include "game/object.h"
#include "game/palette.h"
#include "game/party.h"
#include "game/perk.h"
#include "game/proto.h"
#include "game/inventry.h"
#include "game/protinst.h"
#include "game/roll.h"
#include "game/scripts.h"
#include "game/select.h"
#include "game/skill.h"
#include "game/stat.h"
#include "game/textobj.h"
#include "game/tile.h"
#include "game/worldmap.h"
#include "plib/color/color.h"
#include "plib/db/db.h"
#include "plib/gnw/button.h"
#include "plib/gnw/debug.h"
#include "plib/gnw/gnw.h"
#include "plib/gnw/input.h"
#include "plib/gnw/kb.h"
#include "plib/gnw/mouse.h"
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
    COOP_MSG_WORLDMAP_BEGIN = 22, // host -> client, the host opened the world map screen (host-only travel)
    COOP_MSG_WORLDMAP_STATE = 23, // host -> client, rough travel progress while the world map screen is open
    COOP_MSG_WORLDMAP_END = 24, // host -> client, the host left the world map screen
    COOP_MSG_GAME_TIME = 25, // host -> client, the host's current in-game clock
    COOP_MSG_DIALOGUE_VISUAL_BEGIN = 26, // host -> client, the real graphical dialogue screen (background/portrait) opened
    COOP_MSG_DIALOGUE_VISUAL_END = 27, // host -> client, the real graphical dialogue screen closed
    COOP_MSG_INVENTORY_AP_REQUEST = 28, // client -> host, "I opened the companion's inventory during its own turn, deduct the usual AP cost"
    COOP_MSG_COMBAT_TEXT = 29, // host -> client, a real combat log line (miss/hit/damage/crit/death) captured while the companion was attacker or defender
    COOP_MSG_COMBAT_ATTACK_ANIM = 30, // host -> client, "this object just began its attack swing/fire animation" (item_w_anim's result), so the client sees the actual attack motion play out
    COOP_MSG_COMBAT_DAMAGE_ANIM = 31, // host -> client, mirrors one show_damage_to_object() call verbatim so the client plays the real hit-flinch/knockback/death animation instead of just silently applying the HP change
    COOP_MSG_OBJECT_ANIM = 32, // host -> client, same wire shape/handling as COMBAT_ATTACK_ANIM (a one-shot register_object_animate) but for non-combat gestures -- picking an item up off the ground, reaching to use a door/scenery object
    COOP_MSG_SCENERY_STATE = 33, // host -> client, a door (identified by pid/tile/elevation, same convention as CoopItemEvent) just opened or closed on the host's authoritative world
    COOP_MSG_COMBAT_START_REQUEST = 36, // client -> host, "start a fight" -- payload is a CoopItemEvent (pid/tile/elevation) naming the clicked target, or tile = -1 for none (the A key)
    COOP_MSG_TIME_ADVANCE = 35, // client -> host, the client rested (pipboy) -- carries its new absolute game time (CoopGameTime), host adopts it if later than its own
    COOP_MSG_MOVE_ANIM = 34, // host -> client, the companion/obj_dude was just told to walk or run to a destination tile (sent once per path, not per step)
    COOP_MSG_REMOTE_BEGIN = 42, // host -> client, a screen only the host can run (barter, "tell me about"...) is now being driven by the client: show the host's screen
    COOP_MSG_REMOTE_TILE = 43, // host -> client, one 32x32 block of the host's screen that changed
    COOP_MSG_REMOTE_PALETTE = 44, // host -> client, the host screen's 256-colour palette
    COOP_MSG_REMOTE_END = 45, // host -> client, the remote screen is over
    COOP_MSG_REMOTE_INPUT = 46, // client -> host, the driver's mouse position/buttons and/or one key press/release
    COOP_MSG_RESYNC_REQUEST = 53, // client -> host, "I just loaded a save: send me the map, companion and world again"
    COOP_MSG_ATTACK_SFX = 52, // host -> client, an attacker's sound effect (name + delay) to play locally
    COOP_MSG_WORLD_REMOVE = 50, // host -> client, a streamed critter is gone on the host (drop the local copy)
    COOP_MSG_WORLD_ITEM = 51, // host -> client, a loose ground item exists / is gone at (pid, tile, elevation)
    COOP_MSG_USE_ITEM = 49, // client -> host, "my companion uses this inventory item" (pid; optionally on the host's character)
    COOP_MSG_LOOT_REQUEST = 48, // client -> host, "my companion loots this corpse" (pid/tile/elevation of the critter)
    COOP_MSG_INVENTORY_PUSH = 47, // client -> host, full snapshot of the companion's inventory + equipped items after the client closed its inventory window
    COOP_MSG_FLOAT_TEXT = 41, // host -> client, a floating message above an object (NPC barks, script float_msg, combat taunts) -- see CoopFloatText
    COOP_MSG_DIALOGUE_START_REQUEST = 38, // client -> host, "I want to talk to this NPC" (CoopItemEvent pid/tile/elevation) -- the client that starts a conversation DRIVES it
    COOP_MSG_DIALOGUE_PICK = 39, // client -> host, the driver picked dialogue option N (int32, 0-based)
    COOP_MSG_DIALOGUE_DRIVER = 40, // host -> client, one byte: 1 = the client drives this conversation (option picks are accepted), 0 = the client only watches
    COOP_MSG_GVAR_DELTA = 37, // host -> client, a batch of (index, value) global-variable entries (quest/story/karma state) -- variable length, see CoopGvarDelta
    COOP_MSG_SETTINGS = 60, // host -> client, the game/combat difficulty the host is running (the host owns the simulation, so its values win) -- see CoopSettings
    COOP_MSG_CHARACTER = 61, // client -> host, the client's own character (name, SPECIAL, skill levels) -- see CoopCharacter
    COOP_MSG_CHAR_BLOB = 62, // client -> host, the client's whole saved character (progress included) to be kept with the host's save -- see CoopCharBlob
    COOP_MSG_CHAR_RESTORE = 63, // host -> client, "this is how far that character got in MY world" -- the client applies it -- see CoopCharBlob
    COOP_MSG_XP = 64, // host -> client, experience the party just earned -- see CoopXp
    COOP_MSG_HEAD_FRAME = 65, // host -> client, the talking-head frame the host just drew -- see CoopHeadFrame
};

// Experience the host's party earned (kills and quests alike). Each player keeps
// their own XP total and level; this is just the shared stream of gains, so a
// level-up happens on the client's PC through the normal character screen.
struct CoopXp {
    int32_t amount;
};

// The talking head is animated at random (which fidget, when) and follows the
// voice's phonemes, so two copies never agree. The host's drawn frames are sent
// instead: which head art (fid) and which frame of it.
struct CoopHeadFrame {
    int32_t fid;
    int32_t frame;
};

// A whole character, in the game's own character-file format (stats, skills,
// traits, level/XP, perks -- see pc_coop_save_data()), plus the few fields the
// host needs to decide what to do with it without parsing the file.
const int kCoopCharBlobMax = 1400;

struct CoopCharBlob {
    uint8_t kind; // client -> host: 0 = "this is who I am" (join), 1 = "I changed" (level up, skills...)
    char name[32];
    int32_t level;
    int32_t xp;
    int32_t blobLen;
    uint8_t blob[kCoopCharBlobMax];
};

// The client's own character, sent right after connecting. The numbers are the
// EFFECTIVE values from the client's own game (traits, tags and perks already
// folded in), because the companion on the host is an ordinary critter that
// only knows plain stats and skill points.
struct CoopCharacter {
    char name[32];
    int32_t special[7];
    int32_t skills[18];
    int32_t level;
    // Max HP gained from levelling up (the game stores it as a bonus on top of
    // the stat-derived value, so it isn't visible in SPECIAL alone).
    int32_t hpBonus;
};

// Game and combat difficulty change damage, AI and skill rolls, all of which
// the HOST simulates -- so its values are the real ones and the client's local
// copy simply follows them (a client changing its own would otherwise only
// desync its own non-authoritative simulation).
struct CoopSettings {
    int32_t gameDifficulty;
    int32_t combatDifficulty;
    // Animation pace: the host's combat speed setting. Left to each player's own
    // preference, the two screens played the same fight at different speeds and
    // a replayed animation was cut off by the next message before it finished.
    int32_t combatSpeed;
    // Whether the host's own character also gets the combat walking speed-up
    // (the "player speedup" preference) -- it applies to the host's character as
    // seen on the client too.
    int32_t playerSpeedup;
};

// Bumped whenever behavior changes in a way an older exe on the other side
// would misread -- a mismatch is now refused with a clear message (see the
// HELLO handling) instead of producing confusing half-working sessions.
const uint32_t kCoopProtocolVersion = 3;

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
    // Same walk-vs-run decision vanilla makes for obj_dude's own clicks
    // (the "running" preference XOR Shift, gmouse.cc), computed on the
    // client from its own config/keyboard -- the host used to always run,
    // so the client's character never walked even in walking mode.
    uint8_t run;
};

struct CoopPosition {
    uint8_t which; // 0 = companion, 1 = obj_dude
    int32_t tile;
    int32_t elevation;
    int32_t rotation;
    // Current hit points. Confirmed via user testing as a real gap: the
    // client's local mirror objects for the companion/obj_dude never had
    // combat damage applied to them at all -- only position moved, so
    // getting hit never visibly cost HP on the client's screen (the
    // client's own character sheet/interface bar reads its own local
    // object's stats, which never changed). See
    // coopnet_client_apply_position()'s comment for how this gets applied.
    int32_t hp;
    // What the character looks like it's holding/wearing: the fid's base art
    // number (FID & 0xFFF -- changes with armor) and weapon animation code
    // ((FID & 0xF000) >> 12 -- changes with the wielded weapon). Equipped
    // state was never synced at all, so the client only saw the right
    // weapon/armor after opening the inventory (confirmed via testing). The
    // client keeps its own current animation type and facing and only swaps
    // these two parts -- see coopnet_client_apply_position().
    int32_t fidBase;
    int32_t weaponCode;
    int32_t transFlags; // OBJECT_TRANS_* bits: stealth boy / invisibility look
};

struct CoopUseItem {
    int32_t pid;
    uint8_t onHostDude;
};

struct CoopAttackSfx {
    int32_t delay;
    char name[24];
};

// Host -> client world sync: a streamed critter no longer exists on the host.
struct CoopWorldRemove {
    int32_t id;
    int32_t pid;
};

// Host -> client world sync: a loose ground item exists (present=1) or is gone
// (present=0) at (pid, tile, elevation).
struct CoopWorldItem {
    int32_t pid;
    int32_t tile;
    int32_t elevation;
    uint8_t present;
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
    // object it maps to). targetId == -2 means "obj_dude specifically" --
    // deliberate friendly fire, requested by clicking the host's sprite --
    // see coopnet_on_client_attack()'s comment for why obj_dude needs its
    // own sentinel rather than going through the normal participant-id
    // lookup (obj_dude is deliberately never in that table). Otherwise
    // (targetId == -1, or an id the host doesn't recognize -- e.g. it died
    // before the click round-tripped) falls back to combat_ai()'s own
    // auto-targeting, same as before participant mirroring existed.
    COOP_COMBAT_ACTION_ATTACK = 2,
};

const int32_t kCoopCombatTargetHostDude = -2;

// Same sentinel idea as kCoopCombatTargetHostDude, for the companion --
// used by the attack/damage animation messages below to identify "the
// companion" without needing a real participant-table entry (the
// companion, like obj_dude, is deliberately never added to
// g_coopParticipants -- it already has its own dedicated sync).
const int32_t kCoopAnimIdCompanion = -3;

struct CoopCombatAction {
    uint8_t actionType;
    int32_t targetTile; // valid when actionType == COOP_COMBAT_ACTION_MOVE
    int32_t targetId; // valid when actionType == COOP_COMBAT_ACTION_ATTACK; -1 = auto-target, kCoopCombatTargetHostDude = obj_dude
    int32_t hitMode; // attack: the interface's chosen attack (hand + primary/secondary), HIT_MODE_*; -1 = default
    int32_t hitLocation; // attack: called-shot body part, HIT_LOCATION_*; uncalled unless the client aimed
    int32_t targetPid; // attack with targetId == -1: pid + targetTile of the clicked critter (client's world is unsynced, so the host searches for the nearest match); -1 = none
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
    int32_t fid; // host's current art id: body/armor + weapon code for the look, full fid for a corpse first seen dead
    uint8_t resync; // 1 = periodic "still here" refresh of a critter that has NOT moved: the client snaps to it if its copy drifted
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
    int32_t flags; // OBJECT_IN_LEFT_HAND / OBJECT_IN_RIGHT_HAND / OBJECT_WORN bits (which item is equipped where)
    int32_t dataA; // weapon: loaded ammo count; ammo: rounds in the top box; misc: charges
    int32_t dataB; // weapon: loaded ammo type pid
};

struct CoopInventorySync {
    uint8_t itemCount;
    uint8_t activeHand; // which hand the companion is currently using (interface item slot: 0 = left, 1 = right)
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
    // True for the "walked into a building, same map file, just a
    // different elevation" case (see g_coopHostLastElevation's comment).
    // The client skips the full map_load()/companion-respawn cycle for
    // this case -- the map is already loaded, only the elevation/position
    // need to move.
    uint8_t sameMapElevationOnly;
};

// Host -> client: a read-only mirror of the host's current dialogue screen
// -- the client can never initiate or affect dialogue itself (see
// coopnet_notify_dialogue_state()'s comment and gmouse.cc's Client-role
// guard on the talk click), this is purely so the client can watch what the
// host is doing. Text is copied out of gdialog.cc's own already-resolved
// display buffers (dialogBlock.replyText / dialogBlock.options[i].text --
// real text ready to show, not a message-list id the client would need to
// look up itself) and truncated to fit a small, fixed wire format.
//
// Also carries the same (messageListId, messageId) pairs (plus, per
// option, the empathy-perk `reaction` code) that produced that text on
// the host -- unused by the plain-text mirror window, but read by the
// real-visual puppet (see coopnet_notify_dialogue_visual_begin()'s
// comment in coopnet.h) to re-run the exact same local lookup, which is
// also what triggers voice audio. -4 means "no message-list entry, the
// text was supplied literally" (gdAddOptionStr()'s own sentinel) --
// same convention gdialog.cc's own dialogBlock already uses, passed
// through unchanged rather than inventing a different one.
const int kCoopMaxDialogueOptions = 6;
const int kCoopDialogueTextLen = 180;

struct CoopDialogueState {
    int32_t replyMessageListId;
    int32_t replyMessageId;
    char replyText[kCoopDialogueTextLen];
    uint8_t optionCount;
    int32_t optionMessageListId[kCoopMaxDialogueOptions];
    int32_t optionMessageId[kCoopMaxDialogueOptions];
    int32_t optionReaction[kCoopMaxDialogueOptions];
    char optionText[kCoopMaxDialogueOptions][kCoopDialogueTextLen];
};

// Host -> client: scr_dialogue_init()'s own two parameters, passed through
// unchanged when the host's NPC script brings up the real graphical
// dialogue screen (see coopnet_notify_dialogue_visual_begin()'s comment
// in coopnet.h for why this is a separate, later event than the plain
// COOP_MSG_DIALOGUE_BEGIN).
struct CoopDialogueVisualBegin {
    int32_t headFid;
    int32_t reaction;
    // Where the speaker is (-1 if unknown). The client's own camera and roof
    // were left wherever they happened to be, so a headless NPC's "who am I
    // talking to" view showed some unrelated patch of ground.
    int32_t targetTile;
    int32_t targetElevation;
    // The talking-head background the NPC's script chose (start_gdialog's last
    // argument). The client never runs that script, so it always showed its
    // default one.
    int32_t background;
};

// Host -> client: the shared game has ended (see coopnet_notify_game_over()'s
// comment in coopnet.h for why the companion dying counts as much as the
// host dying).
struct CoopGameOver {
    uint8_t reason;
};

// Host -> client: rough progress while the host is on the world map screen
// (see COOP_MSG_WORLDMAP_BEGIN/_END) -- the client can never open this
// screen itself (host-only travel, see scripts_request_worldmap()'s
// comment), so without this its screen would just sit frozen with zero
// feedback for however long the host spends traveling. Deliberately coarse
// (terrain type + moving/stopped), not exact coordinates -- mirroring the
// real pixel worldmap/moving dot would mean re-sending the whole graphical
// screen, a much bigger feature (same scoping call as the dialogue mirror,
// see CoopDialogueState's comment).
struct CoopWorldmapState {
    uint8_t terrain; // TerrainType, see worldmap.h
    uint8_t isMoving;
};

// Host -> client: the host's current in-game clock (game_time(), tenths of a
// second of game time -- same unit set_game_time()/game_time() already use).
// The client runs its own fully independent simulation with its own clock,
// which only ever advances by however much real-time gameplay happens
// locally on the client's own screen -- it never sees the large jumps the
// host's clock takes from worldmap travel (each tile of travel advances game
// time by a lot more than the seconds it takes to walk it), so the two
// clocks drift apart over any real session. Confirmed via testing: host and
// client ended up on opposite sides of day/night after a single worldmap
// trip. Periodic + diffed (see coopnet_host_broadcast_game_time()), not
// sent every tick, since it only actually changes in relatively large jumps.
struct CoopGameTime {
    int32_t gameTime;
};

// Host -> client: one real combat log line, verbatim, captured from
// combat_display()'s own display_print() calls (combat.cc) while the
// companion was attacker or defender -- see coopnet_begin_capture_combat_text()'s
// comment for how the capture itself works. Carries the host's own
// already-resolved text (same reasoning as CoopDialogueState -- an ID-based
// re-lookup would need the same message-list index/id plumbing dialogue
// has, not worth it just for combat flavor text), so it always matches the
// host's own wording/language exactly, at the cost of not respecting the
// client's own language if it differs from the host's -- a known,
// accepted limitation, same as the other short coop notification strings
// already added this project.
const int kCoopCombatTextLen = 200;

struct CoopCombatText {
    char text[kCoopCombatTextLen];
};

// Host -> client: an object (companion or obj_dude, id per
// kCoopAnimIdCompanion/kCoopCombatTargetHostDude) just began the attack
// animation for its current hit mode -- `anim` is item_w_anim()'s result,
// the same code action_attack() itself computes to decide melee vs
// ranged. Sent from action_attack()'s own top (actions.cc), the single
// dispatcher every attack in the game funnels through, so this can't miss
// a call site the way the first combat-text attempt did (see the
// SESSION 7 note on that bug).
struct CoopCombatAttackAnim {
    int32_t attackerId;
    int32_t anim;
    // Direction the attacker faces for this attack (0-5), -1 = leave as is.
    int32_t facing;
};

// Host -> client: mirrors one real show_damage_to_object() call (actions.cc)
// verbatim -- that function is the engine's own single choke point for
// playing a critter's damage-reaction animation (flinch/knockback/death,
// blood, corpse conversion), driven entirely by already-decided outcome
// data (no RNG or game logic of its own), which is exactly why it's safe
// to just replay on the client with the same arguments. `weapon` is
// deliberately not carried over (only affects a rare weapon-explodes-on-
// defender edge case) and `attacker` is resolved to a real local object
// on the client, falling back to the defender itself if unresolvable
// (attacker is only used for a couple of comparisons/knockback direction
// math, so a fallback is harmless for what's a purely visual replay).
struct CoopCombatDamageAnim {
    int32_t attackerId; // -1 if not companion/obj_dude/a tracked participant
    int32_t defenderId; // always companion or obj_dude -- see the send-side gate
    int32_t damage;
    int32_t flags;
    int32_t knockbackDistance;
    int32_t knockbackRotation;
    int32_t anim;
    int32_t delay;
    uint8_t hitFromFront;
};

// Host -> client: identifies a door (or any openable scenery -- see
// obj_is_openable()) by (pid, tile, elevation), same convention as
// CoopItemEvent, plus whether it just ended up open or closed. The
// client's own map already has the identical scenery object loaded (maps
// aren't dynamic), so no spawn/despawn bookkeeping is needed the way
// combat participants or the companion need -- just find it and apply the
// state via the same real obj_open()/obj_close() the host used.
struct CoopSceneryState {
    int32_t pid;
    int32_t tile;
    int32_t elevation;
    uint8_t isOpen;
};

// Host -> client: a batch of global-variable values (game_global_vars[] --
// where quests, story flags, karma, reputation and the like live). The host is
// the only real simulation, so these are authoritative and the client just
// writes them into its own array. Variable length: the wire payload is
// 1 + count * sizeof(CoopGvarEntry) bytes (the array can hold hundreds of
// variables, more than fits in one message, so it goes out in batches).
const int kCoopGvarMaxEntries = 100;

struct CoopGvarEntry {
    int32_t index;
    int32_t value;
};

struct CoopGvarDelta {
    uint8_t count;
    CoopGvarEntry entries[kCoopGvarMaxEntries];
};

// "Remote screen": for a modal screen the host runs for real but the CLIENT
// drives (barter, "tell me about", later world map / loot), the client is shown
// the host's actual screen and its mouse and keys are forwarded to the host --
// i.e. control hand-over for any screen, without re-implementing each screen's
// UI on the client. The screen is 8-bit palettised; only 32x32 blocks that
// changed are sent (a mostly static trade window costs almost nothing).
const int kCoopRemoteTile = 32;

struct CoopRemoteBegin {
    int32_t width;
    int32_t height;
    uint8_t viewOnly; // 1 = the client just watches (the host drives), 0 = the client drives
    uint8_t travel; // 1 = the world map / town map screen (the client plays the world map music)
};

struct CoopRemoteTile {
    int16_t tx;
    int16_t ty;
    uint8_t pixels[kCoopRemoteTile * kCoopRemoteTile];
};

struct CoopRemotePalette {
    uint8_t colors[256 * 3];
};

struct CoopRemoteInput {
    int32_t x;
    int32_t y;
    uint8_t buttons; // bit0 left, bit1 right (the engine's raw mouse button bits)
    int32_t scancode; // -1 = no key event in this message
    uint8_t down;
};

// Host -> client: a floating text bubble above an object. Scripts make NPCs
// "bark" with these (float_msg) outside of any conversation, and combat AI
// uses them for taunts -- the client's own scripts no longer run, so without
// this it never saw them (confirmed via testing). The object is identified by
// the same id scheme as the animation messages when that resolves (companion /
// host character / combat participant), otherwise by (pid, tile, elevation)
// and matched to the nearest same-pid critter, like dialogue/combat requests.
const int kCoopFloatTextLen = 100;

struct CoopFloatText {
    int32_t animId;
    int32_t pid;
    int32_t tile;
    int32_t elevation;
    int32_t font;
    int32_t color;
    int32_t a5;
    char text[kCoopFloatTextLen];
};

// Host -> client: the walk/run DESTINATION of the companion or obj_dude
// (id per kCoopAnimIdCompanion/kCoopCombatTargetHostDude), sent once when
// the host registers the move. Replaces the client chasing each ~100ms
// position snapshot with a fresh short run, whose constant start/stop
// looked like running with a limp -- the client now runs the whole path in
// one animation, and CoopPosition only corrects drift.
struct CoopMoveAnim {
    int32_t objId;
    int32_t tile;
    int32_t elevation;
    // A move toward an OBJECT (an enemy closing in on its target) stops
    // adjacent to it rather than on its tile, so the client has to run the same
    // kind of move; -1 = a plain move to `tile`.
    int32_t destObjId;
    // The step limit the host's move was registered with (action points in
    // combat, -1 = unlimited). Without it the client's replay ran the whole
    // way to the destination while the host's critter stopped early.
    int32_t actionPoints;
    uint8_t run;
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
static bool g_coopConnectTargetGiven = false;

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

// Client-side only: the AP count from the most recent COOP_MSG_COMBAT_TURN
// -- the client's own local simulation never actually enters vanilla's real
// combat mode (combat() is host-only, see its role guard in combat.cc), so
// nothing else ever feeds a real AP value into the interface bar for the
// client; this plus g_coopClientCombatTurnActive drive the real interface
// bar's own AP pips/end-turn button panel directly instead (see
// coopnet_client_combat_turn_ui_set_active() and friends, further down).
static int32_t g_coopClientCombatAP = 0;

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

    // The host reported this participant dead and the client has played (or
    // forced) its death animation. Its corpse is deliberately kept in the
    // table -- the host keeps broadcasting a dead participant while the
    // fight lasts, and dropping the entry would just re-adopt/re-spawn it
    // on the next tick -- and is never destroyed (see
    // coopnet_clear_combat_participants()).
    bool dead;

    // Same arrival-based smoothing technique as g_coopLastCommandedTile for
    // the companion/obj_dude (see its comment): only issue a new run once
    // the object has actually reached the last commanded tile, rather than
    // reissuing on every ~100ms broadcast. -1 = nothing commanded yet.
    int lastCommandedTile;

    // A full-path walk/run replayed from the host's own COOP_MSG_MOVE_ANIM is
    // in progress toward this tile (-1 = none), started at moveDestStartMs.
    // While set, the ~100ms position updates leave the critter alone instead
    // of cancelling and re-aiming its run every tick -- that restart-on-every-
    // update is what made enemies stutter and look like they teleported.
    int moveDest;
    uint32_t moveDestStartMs;

    // When this critter was first seen mid-animation that wasn't a replayed
    // walk (a shot, a hit reaction): position updates leave it alone until it
    // finishes, so they can't restart the animation. 0 = not busy.
    uint32_t busySinceMs;
};

const int kCoopMaxParticipants = 200;
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

// Host-side only: true for the narrow window around a single
// combat_display() call (combat.cc) that's relevant to the companion --
// see coopnet_begin_capture_combat_text(). While true, every
// display_print() call anywhere (display.cc has the one hook point) gets
// mirrored to the client as a COOP_MSG_COMBAT_TEXT, verbatim, in the same
// order the host's own screen shows them (a single attack can print
// several lines -- miss, then a follow-up, etc.).
static bool g_coopCapturingCombatText = false;

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

static void coopnet_host_apply_dialogue_start(const CoopItemEvent& evt);
struct CoopFloatText;
static void coopnet_client_apply_float_text(const CoopFloatText& f);

// Client-side only: destination of the full-path walk/run most recently
// started from a COOP_MSG_MOVE_ANIM (indexed like g_coopLastCommandedTile),
// -1 when none is in progress. While set, coopnet_client_apply_position()
// leaves the object alone instead of chasing each position snapshot with a
// fresh short run (the stop-start "limp"). Times out so a path that never
// completes (blocked, interrupted) can't suppress correction forever.
static int g_coopMoveDest[2] = { -1, -1 };
static uint32_t g_coopMoveDestStartMs[2] = { 0, 0 };
const uint32_t kCoopMoveDestTimeoutMs = 4000;

// How far a candidate local critter may be from a newly-reported
// participant's tile (or a client-clicked target's tile) and still be
// matched to it, rather than spawning a fresh duplicate / finding nothing --
// NPCs are unsynced, so an exact tile match is too strict. See
// coopnet_apply_combat_participant() and coopnet_host_apply_combat_start().
const int kCoopParticipantAdoptDistance = 20;

// See coopnet_block_local_move()'s comment.
static int g_coopSanctionedMoveDepth = 0;

// Host side: when the client last ordered the companion to walk somewhere.
static uint32_t g_coopHostLastMoveIntentMs = 0;

// Client side: true while a host-ordered map load is in progress and
// g_coopCompanion is in a half torn-down state (kept alive by OBJECT_NO_REMOVE
// but not yet respawned for the new map). Camera limits, the reachability
// cursor, roof/viewpoint logic and the interface bar all follow the client's
// companion, and dereferencing it in that window crashed the client on connect
// (a runaway render recursion on a corrupted object, from a crash dump).
static bool g_coopClientMapLoading = false;

// The companion to use for anything "view" related (camera, cursor, roof, HUD).
// NULL during a map load -- callers fall back to obj_dude.
Object* coopnet_get_view_companion()
{
    return g_coopClientMapLoading ? NULL : g_coopCompanion;
}

// DEBUG: art-cache heap validation after each client message, see the loop
// in coopnet_poll_client(). Turns itself off after the first hit.
static int g_coopHeapCheckLastType = -1;
// Off: validating the whole art-cache heap after every message became a heavy
// per-frame cost once the world sync streams dozens of messages a second
// (client at ~5 fps). Nothing has reproduced the corruption since.
static bool g_coopHeapCheckEnabled = false;

static void coopnet_client_heap_check(const char* where)
{
    if (!g_coopHeapCheckEnabled) {
        return;
    }
    if (!heap_validate(&art_cache.heap)) {
        debug_printf("\nCoop: ART HEAP CORRUPT detected at %s -- last handled message type=%d\n", where, g_coopHeapCheckLastType);
        g_coopHeapCheckEnabled = false;
    }
}

// "Whoever starts a modal screen drives it; the other player watches" -- the
// pattern the Fallout 2 coop project settled on, adopted here for dialogue
// first (world travel and location exits are meant to reuse it).
//
// Host side: a client talk request only QUEUES the conversation (through
// scripts_request_dialog(), run later by the main loop), so the "client will
// drive" fact is held as pending until gdialog_enter() really begins one --
// and expires, so a request the engine refused (NPC can't talk, etc.) can't
// leave the next, host-started conversation wrongly treated as client-driven.
static bool g_coopDialogueDriverPending = false;
static uint32_t g_coopDialogueDriverPendingMs = 0;
static bool g_coopDialogueDrivenByClient = false;
static int g_coopPendingDialoguePick = -1;
const uint32_t kCoopDialogueDriverPendingTimeoutMs = 4000;

// Host-side: set the instant the host's own Talk click starts a conversation
// (talk_to() in actions.cc, a1 == obj_dude) -- i.e. the one case where who's
// driving is already known for certain, not guessed. DIALOGUE_SYSTEM_ENTER
// (op_dialogue_system_enter) runs as the *first opcode of every single NPC's
// talk_p_proc*, including ones opened by this very click -- not just genuine
// NPC-initiated ambient greetings, which is the only case
// coopnet_mark_dialogue_client_initiated()'s "whoever's physically closer"
// guess was actually meant for. Without this, that guess could fire mid-way
// through the host's own ordinary Talk click (the companion routinely stands
// closer to whoever the host is talking to than the host does) and wrongly
// mark the conversation client-driven, which makes DUDE_OBJ resolve to the
// companion for the NPC's entire script (op_dude_obj, intextra.cc) -- for any
// NPC whose dialogue logic checks the "player's" reaction/stats/quest items,
// that silently fails whatever check it was doing and the conversation ends
// immediately with no dialogue ever shown. Confirmed as the mechanism behind
// reports of specific NPCs (e.g. Aradesh) ending dialogue instantly for the
// host despite a normal, un-companion-involved Talk click.
static bool g_coopDialogueHostInitiated = false;

// Client side: the host says this conversation is ours to drive.
static bool g_coopClientDrivesDialogue = false;

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

// Host-side only: obj_dude->elevation as of the last check. A door/stairs
// leading into a small building is often modeled as an elevation change
// on the SAME map file (map_data.name doesn't change at all) rather than
// a real map transition -- coopnet_host_check_map_transition() only ever
// watched the map name, so this exact case (confirmed via testing: host
// walks into a building, client's screen stays on the outside, unaware
// anything happened) went completely unsynced. See that function's
// comment for how this is now also checked. -1 so starting to host
// mid-map is never mistaken for a transition, same reasoning as
// g_coopHostLastMapName's own init.
static int g_coopHostLastElevation = -1;
// Set when the client asked for a full resync after loading one of its own
// saves: the next map "change" is then only a re-send, not a real transition.
static bool g_coopHostResyncOnly = false;

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

// While set, coopnet_send_message() gives up at once (instead of waiting up to
// kCoopSendTimeoutMs) when the socket buffer has no room for the message.
static bool g_coopSendDroppable = false;

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
    COOP_COMPANION_ACTION_LOOT,
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
const uint32_t kCoopCompanionActionTimeoutMs = 15000;

// ---------------------------------------------------------------------------
// Platform socket helpers
// ---------------------------------------------------------------------------

static uint32_t coopnet_now_ms()
{
    return static_cast<uint32_t>(SDL_GetTicks());
}

// Client: attack sounds waiting for their delay (frames of the attack animation).
struct CoopPendingSfx {
    char name[24];
    uint32_t dueMs;
};
static std::vector<CoopPendingSfx> g_coopPendingSfx;

// When each sound name was last actually played (or queued), so a burst of
// identical short blips (rapid drag-drop clicking sends ipickup1/iputdown
// over and over) doesn't pile up faster than the fixed-size sound-effect pool
// can drain -- one clearly audible click beats several that mostly fail to
// load. Small fixed table, oldest entry evicted when full; a name simply not
// being tracked yet behaves as "never played".
struct CoopSfxLastPlayed {
    char name[24];
    uint32_t atMs;
};
const int kCoopSfxDedupeCount = 12;
const uint32_t kCoopSfxDedupeWindowMs = 90;
static CoopSfxLastPlayed g_coopSfxLastPlayed[kCoopSfxDedupeCount];
static int g_coopSfxLastPlayedNext = 0;

// True (and records it) if `name` was played/queued in the last
// kCoopSfxDedupeWindowMs -- caller should skip it.
static bool coopnet_client_sfx_recently_seen(const char* name, uint32_t now)
{
    for (int i = 0; i < kCoopSfxDedupeCount; i++) {
        if (g_coopSfxLastPlayed[i].name[0] != '\0' && strcmp(g_coopSfxLastPlayed[i].name, name) == 0) {
            bool recent = now - g_coopSfxLastPlayed[i].atMs < kCoopSfxDedupeWindowMs;
            g_coopSfxLastPlayed[i].atMs = now;
            return recent;
        }
    }
    strncpy(g_coopSfxLastPlayed[g_coopSfxLastPlayedNext].name, name, sizeof(g_coopSfxLastPlayed[0].name) - 1);
    g_coopSfxLastPlayed[g_coopSfxLastPlayedNext].name[sizeof(g_coopSfxLastPlayed[0].name) - 1] = '\0';
    g_coopSfxLastPlayed[g_coopSfxLastPlayedNext].atMs = now;
    g_coopSfxLastPlayedNext = (g_coopSfxLastPlayedNext + 1) % kCoopSfxDedupeCount;
    return false;
}

static void coopnet_client_run_pending_sfx()
{
    uint32_t now = coopnet_now_ms();
    for (size_t i = 0; i < g_coopPendingSfx.size();) {
        if (now >= g_coopPendingSfx[i].dueMs) {
            int rc = gsound_play_sfx_file(g_coopPendingSfx[i].name);
            debug_printf("\nCoop: playing attack sound %s -> %d\n", g_coopPendingSfx[i].name, rc);
            g_coopPendingSfx.erase(g_coopPendingSfx.begin() + i);
        } else {
            i++;
        }
    }
}

static void coopnet_sockets_init()
{
    if (g_coopSocketsInitialized) {
        return;
    }

#ifdef _WIN32
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
#else
    // send() on a socket the peer already closed raises SIGPIPE on
    // macOS/Linux, which would kill the whole game the moment the other
    // player disconnects. Windows has no such signal.
    signal(SIGPIPE, SIG_IGN);
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

static int coopnet_last_socket_error()
{
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

// ---------------------------------------------------------------------------
// Connection status log -- the F9 co-op menu (coopnet_open_menu(), further
// down) shows this live, on both the host and the client, so a failed
// connection says WHY (nobody listening, packets blocked, version mismatch...)
// instead of silently doing nothing, which is all F9 + a debug log could do.
// ---------------------------------------------------------------------------

enum CoopStatusSeverity {
    COOP_STATUS_INFO = 0,
    COOP_STATUS_GOOD = 1,
    COOP_STATUS_WARN = 2,
    COOP_STATUS_BAD = 3,
};

const int kCoopStatusMaxLines = 48;
const int kCoopStatusLineLen = 120;

struct CoopStatusLine {
    char text[kCoopStatusLineLen];
    int severity;
    uint32_t ms;
};

static CoopStatusLine g_coopStatusLines[kCoopStatusMaxLines];
static int g_coopStatusTotal = 0;
static bool g_coopMenuOpen = false;

static uint32_t coopnet_now_ms();

// coop_connection.log: every connection event with a real date and time,
// APPENDED across game launches. coopnet_debug.log is rewritten on every
// start, so a failed attempt was gone the moment the player relaunched to try
// again -- and "it didn't connect yesterday" is exactly what needs a record.
static void coopnet_connection_log(int severity, const char* text)
{
    static bool started = false;
    if (!started) {
        started = true;
        // Don't let it grow forever: past ~256 KB start over.
        FILE* probe = fopen("coop_connection.log", "rb");
        if (probe != NULL) {
            fseek(probe, 0, SEEK_END);
            long size = ftell(probe);
            fclose(probe);
            if (size > 256 * 1024) {
                FILE* reset = fopen("coop_connection.log", "wb");
                if (reset != NULL) {
                    fclose(reset);
                }
            }
        }

        FILE* header = fopen("coop_connection.log", "ab");
        if (header != NULL) {
            time_t now = time(NULL);
            struct tm* local = localtime(&now);
            char stamp[32] = "";
            if (local != NULL) {
                strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", local);
            }
            fprintf(header, "\n==== game started %s - build %s %s, protocol %u ====\n", stamp, __DATE__, __TIME__, kCoopProtocolVersion);
            fclose(header);
        }
    }

    FILE* f = fopen("coop_connection.log", "ab");
    if (f == NULL) {
        return;
    }

    time_t now = time(NULL);
    struct tm* local = localtime(&now);
    char stamp[32] = "";
    if (local != NULL) {
        strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", local);
    }
    const char* tag = severity == COOP_STATUS_BAD ? "ERROR" : (severity == COOP_STATUS_WARN ? "WARN " : (severity == COOP_STATUS_GOOD ? "OK   " : "info "));
    fprintf(f, "%s %s %s\n", stamp, tag, text);
    fclose(f);
}

static void coopnet_status(int severity, const char* fmt, ...)
{
    char text[kCoopStatusLineLen];
    va_list args;
    va_start(args, fmt);
    vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);

    CoopStatusLine& line = g_coopStatusLines[g_coopStatusTotal % kCoopStatusMaxLines];
    snprintf(line.text, sizeof(line.text), "%s", text);
    line.severity = severity;
    line.ms = coopnet_now_ms();
    g_coopStatusTotal++;

    debug_printf("\nCoop-status: %s\n", text);
    coopnet_connection_log(severity, text);

    // With the menu closed the player would otherwise never see it.
    if (!g_coopMenuOpen && severity != COOP_STATUS_INFO) {
        display_print(text);
    }
}

// Host-side: every IPv4 address another player could try, labelled by kind
// (Radmin/Hamachi/home network) so "which one do I give my friend?" has an
// obvious answer. Link-local (169.254.x.x) and loopback are skipped.
static int coopnet_collect_local_ips(char out[][64], int maxCount)
{
    int count = 0;
#ifdef _WIN32
    ULONG size = 0;
    const ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    GetAdaptersAddresses(AF_INET, flags, NULL, NULL, &size);
    if (size == 0) {
        return 0;
    }

    std::vector<unsigned char> buffer(size);
    IP_ADAPTER_ADDRESSES* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
    if (GetAdaptersAddresses(AF_INET, flags, NULL, adapters, &size) != NO_ERROR) {
        return 0;
    }

    for (IP_ADAPTER_ADDRESSES* adapter = adapters; adapter != NULL && count < maxCount; adapter = adapter->Next) {
        if (adapter->OperStatus != IfOperStatusUp || adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) {
            continue;
        }

        for (IP_ADAPTER_UNICAST_ADDRESS* ua = adapter->FirstUnicastAddress; ua != NULL && count < maxCount; ua = ua->Next) {
            sockaddr_in* sin = reinterpret_cast<sockaddr_in*>(ua->Address.lpSockaddr);
            if (sin == NULL || sin->sin_family != AF_INET) {
                continue;
            }

            const unsigned char* b = reinterpret_cast<const unsigned char*>(&sin->sin_addr);
            if (b[0] == 127 || (b[0] == 169 && b[1] == 254)) {
                continue;
            }

            char ip[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof(ip));

            const char* kind = " (internet/other)";
            if (b[0] == 26) {
                kind = " (Radmin VPN)";
            } else if (b[0] == 25) {
                kind = " (Hamachi)";
            } else if (b[0] == 10 || (b[0] == 192 && b[1] == 168) || (b[0] == 172 && b[1] >= 16 && b[1] <= 31)) {
                kind = " (home network)";
            }

            snprintf(out[count], 64, "%s%s", ip, kind);
            count++;
        }
    }
#else
    (void)out;
    (void)maxCount;
#endif
    return count;
}

// ---------------------------------------------------------------------------
// Glitch log -- every place the sync code notices something going wrong
// (a failed spawn, a dropped message, a recovered stale state...) reports it
// here, so it shows up on the co-op menu's "Glitches" page and in the saved
// report instead of living only in a debug log nobody opens. Identical
// consecutive entries are folded into a repeat count so one stuck condition
// can't push everything else out.
// ---------------------------------------------------------------------------

const int kCoopGlitchMaxLines = 160;

struct CoopGlitchLine {
    char text[kCoopStatusLineLen];
    uint32_t ms;
    int repeat;
};

static CoopGlitchLine g_coopGlitchLines[kCoopGlitchMaxLines];
static int g_coopGlitchTotal = 0;

void coopnet_report_glitch(const char* fmt, ...)
{
    char text[kCoopStatusLineLen];
    va_list args;
    va_start(args, fmt);
    vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);

    // Callers hand over their old debug_printf strings, newlines and all.
    size_t len = strlen(text);
    while (len > 0 && (text[len - 1] == '\n' || text[len - 1] == '\r')) {
        text[--len] = '\0';
    }
    char* start = text;
    while (*start == '\n' || *start == '\r') {
        start++;
    }

    debug_printf("\nCoop-glitch: %s\n", start);

    if (g_coopGlitchTotal > 0) {
        CoopGlitchLine& previous = g_coopGlitchLines[(g_coopGlitchTotal - 1) % kCoopGlitchMaxLines];
        if (strcmp(previous.text, start) == 0) {
            previous.repeat++;
            previous.ms = coopnet_now_ms();
            return;
        }
    }

    CoopGlitchLine& line = g_coopGlitchLines[g_coopGlitchTotal % kCoopGlitchMaxLines];
    snprintf(line.text, sizeof(line.text), "%s", start);
    line.ms = coopnet_now_ms();
    line.repeat = 1;
    g_coopGlitchTotal++;
}

// Client join state: pressing "connect" keeps retrying for a while instead of
// trying once, so the order the two players press their buttons in no longer
// matters (the old behavior failed silently if the client was even a second
// ahead of the host).
const uint32_t kCoopClientRetryWindowMs = 120000;
const uint32_t kCoopClientAttemptTimeoutMs = 7000;
const uint32_t kCoopClientRetryDelayMs = 1500;
const uint32_t kCoopClientAckTimeoutMs = 10000;
// A connection that arrives but never says HELLO would otherwise wedge the
// host in WaitingForHello forever, refusing every later join.
const uint32_t kCoopHostHelloTimeoutMs = 10000;

static char g_coopClientIp[64] = "";
static int g_coopClientPort = 29999;
static bool g_coopClientRetrying = false;
static uint32_t g_coopClientRetryUntilMs = 0;
static uint32_t g_coopClientNextAttemptMs = 0;
static uint32_t g_coopClientAttemptStartMs = 0;
static int g_coopClientAttempts = 0;
static int g_coopClientLastFailCode = 0;

static int g_coopHostIncomingAttempts = 0;
static uint32_t g_coopHostHelloStartMs = 0;
static uint32_t g_coopHostListenStartMs = 0;
static bool g_coopHostNoAttemptWarned = false;

// Human-readable explanation of a failed connect()/SO_ERROR, written for the
// player (this is what the co-op menu shows), with the technical code kept
// at the end for the debug log / bug reports.
static const char* coopnet_explain_connect_error(int err)
{
#ifdef _WIN32
    switch (err) {
    case WSAECONNREFUSED:
        return "the host's PC answered, but the game there isn't hosting yet";
    case WSAETIMEDOUT:
        return "no answer at all - wrong IP, VPN not connected, or a firewall is blocking it";
    case WSAENETUNREACH:
    case WSAEHOSTUNREACH:
        return "that address can't be reached - check the IP and that your VPN is connected";
    case WSAECONNRESET:
    case WSAECONNABORTED:
        return "the connection was reset by the other side";
    default:
        return "connection failed";
    }
#else
    switch (err) {
    case ECONNREFUSED:
        return "the host's PC answered, but the game there isn't hosting yet";
    case ETIMEDOUT:
        return "no answer at all - wrong IP, VPN not connected, or a firewall is blocking it";
    case ENETUNREACH:
    case EHOSTUNREACH:
        return "that address can't be reached - check the IP and that your VPN is connected";
    default:
        return "connection failed";
    }
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
                // Repeating state (screen tiles, positions, the world stream) is
                // simply sent again later: when the peer isn't draining its socket
                // (its game is paused, or sitting in a menu) don't wait for it --
                // the host used to stall two seconds per message and froze.
                // Only before the first byte, so a message is never left half-sent.
                if (g_coopSendDroppable && sent == 0) {
                    return false;
                }
                if (coopnet_now_ms() - startMs > kCoopSendTimeoutMs) {
                    coopnet_report_glitch("send() timed out (buffer never drained), dropping message type=%d\n", type);
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

// Set by coopnet_try_recv_message() when the peer closed the connection or the
// socket failed hard; cleared whenever a new connection is set up.
static bool g_coopPeerClosed = false;

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
            g_coopRecvBufferLen += rc;
        } else if (rc == 0) {
            debug_printf("\nCoop: recv() returned 0 (peer closed connection)\n");
            // Orderly shutdown by peer. Remembered so the poll loops drop the
            // connection NOW instead of waiting out the 2 minute heartbeat
            // timeout (the host sat "waiting for the client's turn" after the
            // client's game was closed).
            g_coopPeerClosed = true;
            return false;
        } else if (!coopnet_would_block()) {
#ifdef _WIN32
            debug_printf("\nCoop: recv() error, WSAGetLastError=%d\n", WSAGetLastError());
#else
            debug_printf("\nCoop: recv() error, errno=%d\n", errno);
#endif
            g_coopPeerClosed = true;
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
static void coopnet_host_apply_saved_profile();
static void coopnet_host_autosave_profile();
static uint32_t g_coopLastProfileAutosaveMs = 0;
void coopnet_host_save_profile(const char* path);

// The companion used to share obj_dude's own pid (0x1000000), which proto_ptr()
// special-cases to ONE shared player prototype -- so the client's character
// always had the host's exact stats and skills, and anything the companion did
// to its own stats (wearing armor adjusts bonus stats, for one) silently
// changed the host's. It now gets a prototype of its own, registered at this
// pid: every stat/skill lookup in the engine goes through proto_ptr(pid), so
// nothing else needs to change. The copy starts out identical to the local
// player prototype; on the client that IS its own character (it loaded its
// own save), and the host overwrites its copy with the client's numbers when
// they arrive (COOP_MSG_CHARACTER).
const int kCoopCompanionPid = 0x10003E7;

static Proto* coopnet_ensure_companion_proto()
{
    Proto* proto = NULL;
    if (proto_ptr(kCoopCompanionPid, &proto) == 0 && proto != NULL) {
        return proto;
    }

    Proto* source = NULL;
    if (proto_ptr(0x1000000, &source) == -1 || source == NULL) {
        return NULL;
    }

    proto = NULL;
    if (proto_find_free_subnode(OBJ_TYPE_CRITTER, &proto) == -1 || proto == NULL) {
        return NULL;
    }

    memcpy(proto, source, sizeof(CritterProto));
    proto->pid = kCoopCompanionPid;
    return proto;
}

// The name shown for the companion (the client's own character name once known).
static char g_coopCompanionName[48] = "Companion";

const char* coopnet_get_companion_name()
{
    return g_coopCompanionName;
}

static Object* coopnet_find_or_spawn_companion(int pid, int tile, int elevation)
{
    if (pid == 0x1000000 || pid == kCoopCompanionPid) {
        if (coopnet_ensure_companion_proto() != NULL) {
            pid = kCoopCompanionPid;
        } else {
            coopnet_report_glitch("Couldn't create the companion's own character data - it will share the host character's stats");
            pid = 0x1000000;
        }
    }

    Object* existing = partyMemberFindObjFromPidStartingAt(pid, 1);
    if (existing != NULL) {
        debug_printf("\nCoop: found existing companion object (pid=%d)\n", pid);
        // See the fresh-spawn path below for why this is needed even on
        // the "found an existing object" path -- cheap and idempotent, and
        // guards against a companion object created by an older build
        // (before this fix existed) still being wrong.
        existing->data.critter.combat.team = 0;
        if (existing->lightIntensity < 0x10000) {
            obj_set_light(existing, 4, 0x10000, NULL);
        }
        return existing;
    }

    Object* companion = NULL;
    if (obj_pid_new(&companion, pid) == -1) {
        coopnet_report_glitch("obj_pid_new failed for pid=%d\n", pid);
        return NULL;
    }

    // The companion shares obj_dude's own pid (0x1000000 -> pc_proto), but
    // combat.team only ever gets explicitly set to 0 (the player's team)
    // for the REAL obj_dude, inside proto_dude_init() -- a one-time setup
    // function that only ever runs for obj_dude itself, never for the
    // companion. obj_pid_new() instead goes through the generic critter
    // proto-copy path (proto_update_init(), proto.cc) which just inherits
    // pc_proto's own raw, never-explicitly-set team field -- not
    // necessarily 0. Confirmed via user testing as the likely cause of
    // "enemies don't attack the companion": AI targeting (ai_danger_source(),
    // ai_find_nearest_team(), combatai.cc) is team-based, so a companion
    // that isn't actually on the player's team doesn't get treated as a
    // real ally-in-a-fight target the same way obj_dude does.
    companion->data.critter.combat.team = 0;

    // The companion is built from obj_dude's own proto, whose fid can carry
    // the host character's wielded-weapon animation bits ((fid & 0xF000) >> 12)
    // -- but its inventory starts empty, so it stood holding a weapon it
    // doesn't have (the "phantom SMG", confirmed via testing: sprite holds an
    // SMG, inventory shows nothing). Spawn unarmed; the real wielded-weapon
    // look is synced from the host's companion via CoopPosition.
    if ((companion->fid & 0xF000) != 0) {
        int unarmedFid = art_id(OBJ_TYPE_CRITTER, companion->fid & 0xFFF, FID_ANIM_TYPE(companion->fid), 0, (companion->fid & 0x70000000) >> 28);
        if (art_exists(unarmedFid)) {
            obj_change_fid(companion, unarmedFid, NULL);
        }
    }

    Rect rect;
    obj_move_to_tile(companion, tile, elevation, &rect);
    tile_refresh_rect(&rect, elevation);

    // The player's own character carries a light (object.cc, obj_init) so it
    // can be seen in dark places; the companion -- the client's character --
    // had none, so on dark maps the client could not see himself.
    Rect lightRect;
    if (obj_set_light(companion, 4, 0x10000, &lightRect) != -1) {
        tile_refresh_rect(&lightRect, elevation);
    }

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
            Object* savedItem = inventory->items[i].item;
            saved[i].pid = savedItem->pid;
            saved[i].quantity = inventory->items[i].quantity;
            saved[i].flags = savedItem->flags & OBJECT_EQUIPPED;
            saved[i].dataA = savedItem->data.item.weapon.ammoQuantity;
            saved[i].dataB = item_get_type(savedItem) == ITEM_TYPE_WEAPON ? savedItem->data.item.weapon.ammoTypePid : 0;
        }
    }

    coopnet_destroy_companion(oldCompanion);
    Object* newCompanion = coopnet_find_or_spawn_companion(pid, tile, elevation);

    if (newCompanion != NULL) {
        for (int i = 0; i < savedCount; i++) {
            Object* item = NULL;
            if (obj_pid_new(&item, saved[i].pid) == -1) {
                coopnet_report_glitch("failed to restore companion item pid=%d after respawn\n", saved[i].pid);
                continue;
            }
            item->data.item.weapon.ammoQuantity = saved[i].dataA;
            if (item_get_type(item) == ITEM_TYPE_WEAPON) {
                item->data.item.weapon.ammoTypePid = saved[i].dataB;
            }
            item->flags |= (saved[i].flags & OBJECT_EQUIPPED);
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
            g_coopConnectTargetGiven = true;
        }
    }

    // debug_register_env()/_log()/_mono() are never called anywhere else in
    // this codebase, so debug_printf() is normally a silent no-op. Wire up
    // file logging ourselves -- always, not just under --coop-debug: a player
    // who launches the exe directly (no .bat) and hits a problem still needs
    // a log to send, and the crash handler appends to this same file.
    debug_register_log("coopnet_debug.log", "wt");

    if (g_coopAllowMultipleInstances) {
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
        coopnet_status(COOP_STATUS_BAD, "Can't host: the companion character couldn't be created. Is a game loaded?");
        return false;
    }
    coopnet_host_apply_saved_profile();

    strncpy(g_coopHostLastMapName, map_data.name, sizeof(g_coopHostLastMapName) - 1);
    g_coopHostLastMapName[sizeof(g_coopHostLastMapName) - 1] = '\0';
    g_coopHostLastElevation = obj_dude->elevation;
    g_coopCompanionGameOverSent = false;

    CoopSocket listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSocket == COOP_INVALID_SOCKET) {
        coopnet_status(COOP_STATUS_BAD, "Can't host: the game couldn't create a network socket (error %d).", coopnet_last_socket_error());
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
        int err = coopnet_last_socket_error();
        coopnet_close_socket(listenSocket);
        coopnet_status(COOP_STATUS_BAD, "Can't host: port %d is unavailable (error %d).", port, err);
        coopnet_status(COOP_STATUS_WARN, "Another copy of the game may already be hosting - check Task Manager.");
        return false;
    }

    if (listen(listenSocket, 4) != 0) {
        int err = coopnet_last_socket_error();
        coopnet_close_socket(listenSocket);
        coopnet_status(COOP_STATUS_BAD, "Can't host: listen() failed (error %d).", err);
        return false;
    }

    coopnet_set_nonblocking(listenSocket);

    g_coopListenSocket = listenSocket;
    g_coopRole = CoopRole::Host;
    g_coopConnState = CoopConnState::Listening;
    g_coopLastFollowCheckTimeMs = coopnet_now_ms();
    g_coopLastCommandedTile[0] = -1;
    g_coopHostIncomingAttempts = 0;
    g_coopHostListenStartMs = coopnet_now_ms();
    g_coopHostNoAttemptWarned = false;

    coopnet_status(COOP_STATUS_GOOD, "Hosting on port %d - waiting for your friend to join.", port);

    char ips[6][64];
    int ipCount = coopnet_collect_local_ips(ips, 6);
    if (ipCount > 0) {
        coopnet_status(COOP_STATUS_INFO, "Tell your friend one of these addresses:");
        for (int i = 0; i < ipCount; i++) {
            coopnet_status(COOP_STATUS_INFO, "   %s", ips[i]);
        }
    }
    coopnet_status(COOP_STATUS_INFO, "If he can't connect, the firewall may be blocking the game:");
    coopnet_status(COOP_STATUS_INFO, "run Allow_Firewall.bat once (it asks permission first).");

    return true;
}

static void coopnet_client_attempt_failed(int errCode, const char* detail);

// Written when a join gives up (and at the start of one): which addresses this
// PC has, and whether the address typed in could even be reachable from them.
// The most common causes of "it just doesn't connect" are visible from here --
// typing a Radmin address without Radmin running, or a home-network address
// from a different network -- and nobody checks those by hand.
static void coopnet_diagnose_join_target(const char* target)
{
    char ips[8][64];
    int count = coopnet_collect_local_ips(ips, 8);
    if (count == 0) {
        coopnet_status(COOP_STATUS_WARN, "This PC reports no network address at all - is it online?");
    } else {
        coopnet_status(COOP_STATUS_INFO, "This PC's addresses:");
        for (int i = 0; i < count; i++) {
            coopnet_status(COOP_STATUS_INFO, "   %s", ips[i]);
        }
    }

    unsigned t[4] = { 0, 0, 0, 0 };
    if (sscanf(target, "%u.%u.%u.%u", &t[0], &t[1], &t[2], &t[3]) != 4) {
        return;
    }

    bool haveRadmin = false;
    bool sameNetwork = false;
    for (int i = 0; i < count; i++) {
        unsigned l[4] = { 0, 0, 0, 0 };
        if (sscanf(ips[i], "%u.%u.%u.%u", &l[0], &l[1], &l[2], &l[3]) != 4) {
            continue;
        }
        if (l[0] == 26) {
            haveRadmin = true;
        }
        if (t[0] == 26 && l[0] == 26) {
            sameNetwork = true;
        } else if (t[0] == l[0] && t[1] == l[1] && t[2] == l[2]) {
            sameNetwork = true;
        }
    }

    bool targetPrivate = t[0] == 10 || (t[0] == 192 && t[1] == 168) || (t[0] == 172 && t[1] >= 16 && t[1] <= 31);
    if (t[0] == 127) {
        coopnet_status(COOP_STATUS_INFO, "127.x.x.x only reaches a game running on THIS PC.");
    } else if (t[0] == 26 && !haveRadmin) {
        coopnet_status(COOP_STATUS_WARN, "%s is a Radmin VPN address, but this PC has no Radmin address:", target);
        coopnet_status(COOP_STATUS_WARN, "Radmin VPN isn't running or isn't connected to your friend's network.");
    } else if (t[0] == 26) {
        coopnet_status(COOP_STATUS_INFO, "Radmin looks active here. Check your friend is in the same Radmin network.");
    } else if (targetPrivate && !sameNetwork) {
        coopnet_status(COOP_STATUS_WARN, "%s is a home-network address, but none of this PC's addresses are on", target);
        coopnet_status(COOP_STATUS_WARN, "that network. Not on the same Wi-Fi/router? Use the host's Radmin address instead.");
    }
}

// One TCP connect attempt. Returns false only for a problem retrying can't
// fix (the address can't even be parsed/resolved).
static bool coopnet_client_open_attempt()
{
    CoopSocket sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == COOP_INVALID_SOCKET) {
        coopnet_status(COOP_STATUS_BAD, "The game couldn't create a network socket (error %d).", coopnet_last_socket_error());
        return false;
    }

    coopnet_set_nonblocking(sock);

    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(g_coopClientPort));
    if (inet_pton(AF_INET, g_coopClientIp, &addr.sin_addr) != 1) {
        // Not a plain dotted IPv4 address: resolve it as a hostname instead
        // ("localhost", a computer name...).
        addrinfo hints;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* resolved = NULL;
        int gaiRc = getaddrinfo(g_coopClientIp, NULL, &hints, &resolved);
        if (gaiRc != 0 || resolved == NULL) {
            if (resolved != NULL) {
                freeaddrinfo(resolved);
            }
            coopnet_close_socket(sock);
            coopnet_status(COOP_STATUS_BAD, "\"%s\" isn't a valid IP address or computer name.", g_coopClientIp);
            return false;
        }
        addr.sin_addr = reinterpret_cast<sockaddr_in*>(resolved->ai_addr)->sin_addr;
        freeaddrinfo(resolved);
    }

    g_coopClientAttempts++;
    g_coopClientAttemptStartMs = coopnet_now_ms();
    coopnet_status(COOP_STATUS_INFO, "Attempt %d: connecting to %s:%d ...", g_coopClientAttempts, g_coopClientIp, g_coopClientPort);

    int rc = connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    int connectError = 0;
    bool immediateFailure = false;
    if (rc != 0) {
        connectError = coopnet_last_socket_error();
#ifdef _WIN32
        immediateFailure = connectError != WSAEWOULDBLOCK;
#else
        immediateFailure = connectError != EINPROGRESS;
#endif
    }

    // Non-blocking connect: normally returns immediately with an "in
    // progress" error; actual completion is detected in coopnet_poll().
    g_coopPeerSocket = sock;
    g_coopRole = CoopRole::Client;
    g_coopConnState = CoopConnState::Connecting;
    g_coopPeerClosed = false;
    g_coopRecvBufferLen = 0;
    g_coopCompanion = NULL;
    g_coopLastCommandedTile[0] = -1;
    g_coopLastCommandedTile[1] = -1;

    if (immediateFailure) {
        coopnet_client_attempt_failed(connectError, NULL);
    }

    return true;
}

// Ends the current attempt (socket closed, role reset) and either schedules
// the next one or, once the retry window is spent, gives up with an
// explanation of the most likely causes.
static void coopnet_client_attempt_failed(int errCode, const char* detail)
{
    if (detail != NULL) {
        coopnet_status(COOP_STATUS_WARN, "Attempt %d failed: %s", g_coopClientAttempts, detail);
    } else {
        coopnet_status(COOP_STATUS_WARN, "Attempt %d failed: %s (error %d)", g_coopClientAttempts, coopnet_explain_connect_error(errCode), errCode);
    }
    g_coopClientLastFailCode = errCode;

    coopnet_shutdown();

    uint32_t now = coopnet_now_ms();
    if (g_coopClientRetrying && now < g_coopClientRetryUntilMs) {
        g_coopClientNextAttemptMs = now + kCoopClientRetryDelayMs;
        return;
    }

    g_coopClientRetrying = false;
    coopnet_status(COOP_STATUS_BAD, "Gave up connecting to %s:%d after %d attempts.", g_coopClientIp, g_coopClientPort, g_coopClientAttempts);
    coopnet_diagnose_join_target(g_coopClientIp);

    bool refused = false;
#ifdef _WIN32
    refused = errCode == WSAECONNREFUSED;
#else
    refused = errCode == ECONNREFUSED;
#endif
    if (refused) {
        coopnet_status(COOP_STATUS_WARN, "The host PC is reachable but nobody is hosting. Ask the host");
        coopnet_status(COOP_STATUS_WARN, "to load a save and press F9 first.");
    } else if (detail != NULL) {
        coopnet_status(COOP_STATUS_WARN, "The host's PC accepted the connection but its game never");
        coopnet_status(COOP_STATUS_WARN, "answered. Is the host standing in a loaded game (not a menu)?");
        coopnet_status(COOP_STATUS_WARN, "Both players also need the exact same fallout-ce.exe version.");
    } else {
        coopnet_status(COOP_STATUS_WARN, "Nothing answered. Check: 1) the IP is right, 2) both PCs are on");
        coopnet_status(COOP_STATUS_WARN, "the same Radmin/VPN network, 3) the HOST allowed the game through");
        coopnet_status(COOP_STATUS_WARN, "Windows Firewall (host runs Allow_Firewall.bat once).");
    }
}

bool coopnet_start_client(const char* ip, int port)
{
    if (g_coopRole == CoopRole::Client && g_coopConnState != CoopConnState::Idle) {
        coopnet_status(COOP_STATUS_INFO, "Already connecting/connected.");
        return true;
    }

    coopnet_sockets_init();

    snprintf(g_coopClientIp, sizeof(g_coopClientIp), "%s", ip);
    g_coopClientPort = port;
    g_coopClientAttempts = 0;
    g_coopClientLastFailCode = 0;
    g_coopClientRetrying = true;
    g_coopClientRetryUntilMs = coopnet_now_ms() + kCoopClientRetryWindowMs;

    // Spot an obviously wrong address up front instead of after two minutes.
    coopnet_diagnose_join_target(ip);

    if (!coopnet_client_open_attempt()) {
        g_coopClientRetrying = false;
        return false;
    }

    return true;
}

// Called from coopnet_poll() while no session exists: the next scheduled
// connect attempt, if a join is still being retried.
static void coopnet_client_retry_tick()
{
    if (!g_coopClientRetrying || g_coopRole != CoopRole::None) {
        return;
    }

    if (coopnet_now_ms() < g_coopClientNextAttemptMs) {
        return;
    }

    if (!coopnet_client_open_attempt()) {
        g_coopClientRetrying = false;
    }
}

// Stops everything the player started from the co-op menu (hosting, joining
// or retrying) without quitting the game.
static void coopnet_stop_session()
{
    g_coopClientRetrying = false;
    coopnet_shutdown();
}

// Client-side only, defined further down alongside the rest of the
// dialogue-mirror window code -- forward-declared here so coopnet_shutdown()
// can make sure it never lingers on screen after a disconnect.
static void coopnet_close_dialogue_window();

// Client-side only, defined further down alongside the rest of the
// worldmap-mirror window code -- forward-declared here so coopnet_shutdown()
// can make sure it never lingers on screen after a disconnect.
static void coopnet_close_worldmap_window();

// Client-side only, defined further down alongside the rest of the
// combat-turn UI code -- forward-declared here so coopnet_shutdown() can
// make sure the real end-turn/end-combat button panel never lingers open
// after a disconnect.
static void coopnet_client_combat_turn_ui_end();

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
    coopnet_client_end_dialogue_visual();
    coopnet_close_worldmap_window();
    coopnet_client_combat_turn_ui_end();
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

    // Already heading there in the same gait (e.g. a repeated click while
    // still running toward the same spot) — don't restart the animation
    // for nothing. A same-tile click in a different gait (walk -> run) does
    // restart it, like vanilla's dude_move() -> dude_run() upgrade.
    static uint8_t lastMoveWasRun = 0;
    if (g_coopLastCommandedTile[0] == intent.targetTile && g_coopCompanion->tile != intent.targetTile && lastMoveWasRun == intent.run) {
        return;
    }
    lastMoveWasRun = intent.run;
    g_coopHostLastMoveIntentMs = coopnet_now_ms();

    // Unlike the client's periodic position-apply (which waits for real
    // arrival before redirecting — see coopnet_client_apply_position), a
    // move-intent here represents a discrete, deliberate new click. Vanilla
    // interrupts an in-progress walk immediately on a new click too (see
    // check_move()'s register_clear(obj_dude) for "interrupt walk"), so this
    // does the same rather than waiting — throttling it by time made the
    // companion feel less responsive to clicks than the vanilla player.
    register_clear(g_coopCompanion);
    register_begin(ANIMATION_REQUEST_UNRESERVED);
    if (intent.run) {
        register_object_run_to_tile(g_coopCompanion, intent.targetTile, g_coopCompanion->elevation, -1, 0);
    } else {
        register_object_move_to_tile(g_coopCompanion, intent.targetTile, g_coopCompanion->elevation, -1, 0);
    }
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
// Tells the client why the companion's attack didn't happen, in the game's own
// words (the same messages combat_attack_this() shows a normal player). The
// host used to refuse silently -- the client clicked, nothing happened, and its
// action points sat unchanged with no hint that the shot needs more of them.
static void coopnet_host_explain_refused_attack(int badShot, int hitMode, bool aiming)
{
    int messageId = -1;
    switch (badShot) {
    case COMBAT_BAD_SHOT_NO_AMMO:
        messageId = 101;
        break;
    case COMBAT_BAD_SHOT_OUT_OF_RANGE:
        messageId = 102;
        break;
    case COMBAT_BAD_SHOT_NOT_ENOUGH_AP:
        messageId = 100;
        break;
    case COMBAT_BAD_SHOT_AIM_BLOCKED:
        messageId = 104;
        break;
    case COMBAT_BAD_SHOT_ARM_CRIPPLED:
        messageId = 106;
        break;
    case COMBAT_BAD_SHOT_BOTH_ARMS_CRIPPLED:
        messageId = 105;
        break;
    default:
        return;
    }

    MessageListItem item;
    item.num = messageId;
    if (!message_search(&combat_message_file, &item)) {
        return;
    }

    CoopCombatText msg;
    memset(&msg, 0, sizeof(msg));
    if (messageId == 100) {
        snprintf(msg.text, sizeof(msg.text), item.text, item_w_mp_cost(g_coopCompanion, hitMode, aiming));
    } else {
        snprintf(msg.text, sizeof(msg.text), "%s", item.text);
    }
    coopnet_send_message(g_coopPeerSocket, COOP_MSG_COMBAT_TEXT, &msg, sizeof(msg));
}

static void coopnet_host_apply_combat_attack(int32_t targetId, int32_t targetTile, int32_t targetPid, int32_t clientHitMode, int32_t clientHitLocation);

// A move or attack the client chose that arrived while the companion was still
// animating -- run by coopnet_combat_input()'s loop as soon as it is idle.
static CoopCombatAction g_coopHostPendingCombatAction;
static bool g_coopHostPendingCombatActionValid = false;

static void coopnet_host_run_combat_action(const CoopCombatAction& action)
{
    if (action.actionType == COOP_COMBAT_ACTION_MOVE) {
        debug_printf("\nCoop: received COMBAT_ACTION move targetTile=%d\n", action.targetTile);
        coopnet_host_apply_combat_move(action.targetTile);
    } else if (action.actionType == COOP_COMBAT_ACTION_ATTACK) {
        debug_printf("\nCoop: received COMBAT_ACTION attack targetId=%d\n", action.targetId);
        coopnet_host_apply_combat_attack(action.targetId, action.targetTile, action.targetPid, action.hitMode, action.hitLocation);
    }
}

static void coopnet_host_apply_combat_attack(int32_t targetId, int32_t targetTile, int32_t targetPid, int32_t clientHitMode, int32_t clientHitLocation)
{
    if (g_coopCompanion == NULL) {
        return;
    }

    Object* target = NULL;
    if (targetId == kCoopCombatTargetHostDude) {
        // Deliberate friendly fire -- the client clicked obj_dude's own
        // sprite specifically. obj_dude is never in the participant table
        // (see CoopCombatAction's comment), so this has to be resolved
        // directly rather than through the lookup below.
        target = obj_dude;
    } else if (targetId != -1) {
        int count = combat_get_list_count();
        for (int i = 0; i < count; i++) {
            Object* candidate = combat_get_list_item(i);
            if (candidate != NULL && candidate->id == targetId) {
                target = candidate;
                break;
            }
        }
        if (target == NULL) {
            // Not in the fight yet (a peaceful NPC the client wants to hit):
            // find it anywhere on the map by its id. Without this the attack
            // silently fell back to auto-targeting -- no punch, no sound.
            for (int elevation = 0; elevation < ELEVATION_COUNT && target == NULL; elevation++) {
                for (Object* o = obj_find_first_at(elevation); o != NULL; o = obj_find_next_at()) {
                    if (o->id == targetId && FID_TYPE(o->fid) == OBJ_TYPE_CRITTER && o != obj_dude && o != g_coopCompanion
                        && (o->flags & OBJECT_HIDDEN) == 0 && !critter_is_dead(o)) {
                        target = o;
                        break;
                    }
                }
            }
        }
        if (target == NULL) {
            debug_printf("\nCoop: attack targetId=%d not found (already dead/gone?), falling back to auto-target\n", targetId);
        }
    }

    if (target == NULL && targetId == -1 && targetPid != -1 && targetTile != -1) {
        // Client-side critter isn't a synced participant: nearest living
        // critter of the same pid to where the client saw it.
        int bestDist = kCoopParticipantAdoptDistance + 1;
        // Whole map, not just the combat list: the enemy may not have joined
        // the host's fight yet.
        for (Object* candidate = obj_find_first_at(g_coopCompanion->elevation); candidate != NULL; candidate = obj_find_next_at()) {
            if (candidate->pid != targetPid
                || FID_TYPE(candidate->fid) != OBJ_TYPE_CRITTER
                || candidate == obj_dude || candidate == g_coopCompanion
                || critter_is_dead(candidate)) {
                continue;
            }
            int dist = tile_dist(candidate->tile, targetTile);
            if (dist < bestDist) {
                bestDist = dist;
                target = candidate;
            }
        }
        if (target == NULL) {
            debug_printf("\nCoop: no critter with pid=%d within %d tiles of tile %d on the host (host-only/client-only NPC?)\n", targetPid, kCoopParticipantAdoptDistance, targetTile);
        }
    }

    debug_printf("\nCoop: host attack target=%p (id=%d pid=%d tile=%d)\n", (void*)target, targetId, targetPid, targetTile);
    if (target != NULL) {
        // ONE attack per click. combat_ai() keeps acting until the companion
        // is out of AP ("he keeps attacking until he runs out of AP").
        int hitMode = clientHitMode >= 0 && clientHitMode < HIT_MODE_COUNT ? clientHitMode : HIT_MODE_RIGHT_WEAPON_PRIMARY;
        int hitLocation = clientHitLocation >= 0 && clientHitLocation < HIT_LOCATION_COUNT ? clientHitLocation : HIT_LOCATION_UNCALLED;
        int rc = combat_check_bad_shot(g_coopCompanion, target, hitMode, hitLocation != HIT_LOCATION_UNCALLED);
        debug_printf("\nCoop: host attack hitMode=%d hitLocation=%d check=%d\n", hitMode, hitLocation, rc);
        if (rc == COMBAT_BAD_SHOT_OK) {
            combat_attack(g_coopCompanion, target, hitMode, hitLocation);
        } else if (rc == COMBAT_BAD_SHOT_OUT_OF_RANGE) {
            coopnet_host_explain_refused_attack(rc, hitMode, hitLocation != HIT_LOCATION_UNCALLED);
            register_clear(g_coopCompanion);
            register_begin(ANIMATION_REQUEST_RESERVED);
            register_object_move_to_object(g_coopCompanion, target, g_coopCompanion->data.critter.combat.ap, 0);
            register_end();
        } else {
            coopnet_host_explain_refused_attack(rc, hitMode, hitLocation != HIT_LOCATION_UNCALLED);
        }
        return;
    }

    combat_ai(g_coopCompanion, target);
}

// Host-side only: applies a COOP_MSG_COMBAT_START_REQUEST. The client never
// runs combat() itself (see its role guard in combat.cc), so it could never
// begin a fight on its own -- confirmed via testing. Queues a real scripted
// combat instead (scripts_request_combat(), processed at a safe point by the
// main loop's scripts_check_state(), NOT run from here -- this is called from
// inside coopnet_poll(), which itself runs inside other blocking loops)
// with the companion as the first attacker, so the companion gets the first
// turn and the client picks what it does through the normal turn-gated
// coopnet_combat_input() path. combat_sequence_init() puts the named
// attacker and defender in the initiative list even if the defender wasn't
// hostile, exactly like a scripted ambush.
static void coopnet_host_apply_combat_start(const CoopItemEvent& evt)
{
    if (g_coopCompanion == NULL || isInCombat()) {
        return;
    }

    // NOT an exact-tile match: NPCs are unsynced between the two worlds
    // (each side's AI moves them independently), so the critter the client
    // clicked is almost never on the same tile on the host -- confirmed via
    // testing: an exact match found nothing, the request degraded to a
    // targetless combat(NULL), and no fight ever started. Same reasoning as
    // coopnet_apply_combat_participant()'s adopt-nearest search: take the
    // closest living critter of that pid within kCoopParticipantAdoptDistance.
    Object* target = NULL;
    if (evt.tile != -1) {
        int bestDist = kCoopParticipantAdoptDistance + 1;
        for (Object* object = obj_find_first_at(evt.elevation); object != NULL; object = obj_find_next_at()) {
            if (object->pid != evt.pid
                || FID_TYPE(object->fid) != OBJ_TYPE_CRITTER
                || object == obj_dude || object == g_coopCompanion
                || critter_is_dead(object)) {
                continue;
            }
            int dist = tile_dist(object->tile, evt.tile);
            if (dist < bestDist) {
                bestDist = dist;
                target = object;
            }
        }
    }

    debug_printf("\nCoop: client requested combat start, target=%p (pid=%d tile=%d)\n", (void*)target, evt.pid, evt.tile);

    if (target != NULL) {
        // Same struct scripts use to start an ambush. combat_attack() clamps
        // damage to [minDamage, maxDamage] while it's set (until the first
        // turn ends), so a zeroed struct would zero every hit -- use a range
        // wide enough to change nothing.
        STRUCT_664980 attack;
        memset(&attack, 0, sizeof(attack));
        attack.attacker = g_coopCompanion;
        attack.defender = target;
        attack.minDamage = 0;
        attack.maxDamage = 10000;
        scripts_request_combat(&attack);
    } else {
        // No specific enemy (the A key / weapon-slot click): still put the
        // companion first in the turn order -- plain combat(NULL) starts with
        // whoever the engine sorts first, which was the HOST's character
        // ("the client starts combat but it isn't his turn", confirmed via
        // testing).
        STRUCT_664980 attack;
        memset(&attack, 0, sizeof(attack));
        attack.attacker = g_coopCompanion;
        attack.defender = NULL;
        attack.minDamage = 0;
        attack.maxDamage = 10000;
        scripts_request_combat(&attack);
    }
}

// Client-side only: asks the host to start a fight, optionally against the
// enemy the player clicked. See coopnet_host_apply_combat_start().
void coopnet_on_client_start_combat(Object* target)
{
    debug_printf("\nCoop: start-combat request wanted (target pid=%d, role=%d, state=%d, alreadyInCombat=%d)\n",
        target != NULL ? target->pid : -1, static_cast<int>(g_coopRole), static_cast<int>(g_coopConnState), g_coopClientInCombat ? 1 : 0);
    if (g_coopRole != CoopRole::Client || g_coopConnState != CoopConnState::Connected || g_coopClientInCombat) {
        return;
    }

    CoopItemEvent evt;
    if (target != NULL) {
        evt.pid = target->pid;
        evt.tile = target->tile;
        evt.elevation = target->elevation;
    } else {
        evt.pid = -1;
        evt.tile = -1;
        evt.elevation = g_coopCompanion != NULL ? g_coopCompanion->elevation : 0;
    }

    bool sent = coopnet_send_message(g_coopPeerSocket, COOP_MSG_COMBAT_START_REQUEST, &evt, sizeof(evt));
    debug_printf("\nCoop: sent COMBAT_START_REQUEST target pid=%d tile=%d success=%d\n", evt.pid, evt.tile, sent);
}

static void coopnet_host_broadcast_companion_inventory();

// ---------------------------------------------------------------------------
// Remote screen (see CoopRemoteBegin's comment)
// ---------------------------------------------------------------------------

// Host side.
static bool g_coopRemoteHostActive = false;
// Set around screens the client drives that aren't part of a conversation (loot).
static bool g_coopRemoteForceDrive = false;
static bool g_coopRemoteHostFull = false;
static std::vector<uint8_t> g_coopRemoteShadow;
static std::vector<uint8_t> g_coopRemoteTileSent; // per tile: has it gone out at least once
static std::vector<CoopRemoteInput> g_coopRemoteInputQueue;
static uint8_t g_coopRemotePalShadow[256 * 3];
static uint32_t g_coopRemoteHostLastMs = 0;

// Host-side. Call right before running a screen the client drives (barter,
// "tell me about"); pairs with coopnet_remote_end(). No-op unless the current
// conversation is client-driven.
static bool g_coopRemoteViewOnly = false;

static void coopnet_remote_begin_internal(bool viewOnly, bool travel = false);

void coopnet_remote_begin()
{
    if (g_coopDialogueDrivenByClient || g_coopRemoteForceDrive) {
        coopnet_remote_begin_internal(false);
    }
}

// World-map / town-map / elevator screens: the client ALWAYS sees the host's
// screen; it operates it only when it triggered the trip (walked onto the exit
// itself), otherwise it just watches while the host drives.
static bool g_coopTravelClientDrives = false;
static uint32_t g_coopTravelClientDrivesMs = 0;

bool coopnet_host_companion_exit_allowed(int tile)
{
    // Same as vanilla for the host's own character: stepping on ANY exit grid
    // tile while walking triggers it -- but only while the companion is
    // executing a walk/run the client itself ordered (never while merely
    // following the host, which is what the exact-tile rule was guarding
    // against; it made the client's exits miss the grid whenever the clicked
    // tile had no exit marker on it).
    if (g_coopRole != CoopRole::Host || g_coopConnState != CoopConnState::Connected || isInCombat() || tile == -1
        || g_coopCompanion == NULL) {
        return false;
    }

    // (There used to be a list of maps here -- the Vault 13 ones -- where the
    // client's companion was never allowed to lead an exit, to keep it from
    // walking out of the vault door. It also shut every other transition on
    // those maps for the client, so it is gone: the client can use exit grids
    // everywhere, the Vault 13 ones included.)
    int anim = FID_ANIM_TYPE(g_coopCompanion->fid);
    bool moving = anim == ANIM_WALK || anim == ANIM_RUNNING;
    return moving && g_coopLastCommandedTile[0] != -1 && coopnet_now_ms() - g_coopHostLastMoveIntentMs < 30000;
}

void coopnet_note_client_led_exit()
{
    g_coopTravelClientDrives = true;
    g_coopTravelClientDrivesMs = coopnet_now_ms();
}

// A conversation the client was driving just ended (see coopnet_notify_dialogue_end()):
// a world-map trip that starts right after it is the client's -- the NPC sent
// the party away ("get out of town") as the answer to what the client picked.
static uint32_t g_coopTravelAfterClientDialogueMs = 0;

void coopnet_travel_screen_begin()
{
    bool afterClientDialogue = g_coopTravelAfterClientDialogueMs != 0 && coopnet_now_ms() - g_coopTravelAfterClientDialogueMs < 5000;
    g_coopTravelAfterClientDialogueMs = 0;
    bool clientDrives = (g_coopTravelClientDrives && coopnet_now_ms() - g_coopTravelClientDrivesMs < 15000) || afterClientDialogue;
    g_coopTravelClientDrives = false;
    coopnet_remote_begin_internal(!clientDrives, true);
}

void coopnet_travel_screen_end()
{
    coopnet_remote_end();
}

// Host: pending flag set when the client's companion (not the host's own
// character) just used an item/scenery object -- consumed by whichever screen
// opens as a result. Same pending+timeout idiom as the dialogue driver flag
// above; today the only consumer is the elevator screen (elevator_select() has
// no actor parameter at all to check directly -- see coopnet_elevator_screen_begin()).
static bool g_coopElevatorDriverPending = false;
static uint32_t g_coopElevatorDriverPendingMs = 0;
const uint32_t kCoopElevatorDriverPendingTimeoutMs = 4000;

void coopnet_note_companion_used_object()
{
    g_coopElevatorDriverPending = true;
    g_coopElevatorDriverPendingMs = coopnet_now_ms();
}

static bool coopnet_elevator_driven_by_client()
{
    return g_coopElevatorDriverPending && coopnet_now_ms() - g_coopElevatorDriverPendingMs < kCoopElevatorDriverPendingTimeoutMs;
}

// Elevator selection (elevator.cc's elevator_select(), a blocking modal loop
// like the world map/barter screens) was never shown to the client at all --
// its screen only ever ran on the host, so pressing an elevator floor button
// simply did nothing visible for the client (same class of bug the world map/
// barter/loot screens had before they got the same remote-screen treatment).
void coopnet_elevator_screen_begin()
{
    bool clientDrives = coopnet_elevator_driven_by_client();
    g_coopElevatorDriverPending = false;
    if (g_coopRole == CoopRole::Host && g_coopConnState == CoopConnState::Connected) {
        coopnet_status(COOP_STATUS_INFO, "Elevator screen opened - %s picks the floor.", clientDrives ? "your friend" : "you");
    }
    g_coopRemoteForceDrive = true;
    coopnet_remote_begin_internal(!clientDrives);
}

void coopnet_elevator_screen_end()
{
    g_coopRemoteForceDrive = false;
    coopnet_remote_end();
}

// Host: elevators move the whole party at once -- the client can't walk
// there on its own the way it does for a normal door/exit, so its companion
// has to be explicitly carried along with obj_dude. Without this the
// companion was left behind on the old elevation (confirmed by reading
// coopnet_host_check_map_transition()'s elevation-only branch, which never
// touches the companion -- it only exists to tell the client to re-center its
// camera, not to relocate anyone).
void coopnet_host_move_companion_with_dude()
{
    if (g_coopRole != CoopRole::Host || g_coopCompanion == NULL || obj_dude == NULL) {
        return;
    }
    register_clear(g_coopCompanion);
    Rect rect;
    obj_move_to_tile(g_coopCompanion, obj_dude->tile, obj_dude->elevation, &rect);
    obj_set_rotation(g_coopCompanion, obj_dude->rotation, &rect);
    tile_refresh_rect(&rect, obj_dude->elevation);
}

static void coopnet_remote_begin_internal(bool viewOnly, bool travel)
{
    if (g_coopRole != CoopRole::Host || g_coopConnState != CoopConnState::Connected || gSdlSurface == NULL) {
        return;
    }
    g_coopRemoteViewOnly = viewOnly;

    g_coopRemoteShadow.assign(static_cast<size_t>(gSdlSurface->w) * gSdlSurface->h, 0);
    g_coopRemoteTileSent.assign(static_cast<size_t>((gSdlSurface->w + kCoopRemoteTile - 1) / kCoopRemoteTile) * ((gSdlSurface->h + kCoopRemoteTile - 1) / kCoopRemoteTile), 0);
    g_coopRemoteInputQueue.clear();
    memset(g_coopRemotePalShadow, 0xFF, sizeof(g_coopRemotePalShadow));
    g_coopRemoteHostFull = true;
    g_coopRemoteHostLastMs = 0;
    g_coopRemoteHostActive = true;

    CoopRemoteBegin msg;
    msg.width = gSdlSurface->w;
    msg.height = gSdlSurface->h;
    msg.viewOnly = viewOnly ? 1 : 0;
    msg.travel = travel ? 1 : 0;
    coopnet_send_message(g_coopPeerSocket, COOP_MSG_REMOTE_BEGIN, &msg, sizeof(msg));
    debug_printf("\nCoop: remote screen began (%dx%d) viewOnly=%d\n", msg.width, msg.height, msg.viewOnly);
}

void coopnet_remote_end()
{
    if (!g_coopRemoteHostActive) {
        return;
    }
    g_coopRemoteHostActive = false;
    coopnet_send_message(g_coopPeerSocket, COOP_MSG_REMOTE_END, NULL, 0);
    debug_printf("\nCoop: remote screen ended\n");
}

static void coopnet_remote_host_apply_one_input();

bool coopnet_remote_host_owns_mouse()
{
    return g_coopRemoteHostActive && !g_coopRemoteViewOnly;
}

// Host side, once per presented frame while active (hooked from
// renderPresent()): keeps the network alive (the driver's input arrives
// through it), and sends the palette plus every 32x32 block that changed.
static void coopnet_remote_host_tick()
{
    coopnet_poll();
    coopnet_remote_host_apply_one_input();

    uint32_t now = coopnet_now_ms();
    if (now - g_coopRemoteHostLastMs < 16) {
        return;
    }
    g_coopRemoteHostLastMs = now;

    SDL_Surface* s = gSdlSurface;
    if (s == NULL || s->pixels == NULL) {
        return;
    }

    if (s->format->palette != NULL) {
        CoopRemotePalette pal;
        int n = s->format->palette->ncolors < 256 ? s->format->palette->ncolors : 256;
        memset(&pal, 0, sizeof(pal));
        for (int i = 0; i < n; i++) {
            pal.colors[i * 3] = s->format->palette->colors[i].r;
            pal.colors[i * 3 + 1] = s->format->palette->colors[i].g;
            pal.colors[i * 3 + 2] = s->format->palette->colors[i].b;
        }
        if (memcmp(pal.colors, g_coopRemotePalShadow, sizeof(pal.colors)) != 0) {
            memcpy(g_coopRemotePalShadow, pal.colors, sizeof(pal.colors));
            coopnet_send_message(g_coopPeerSocket, COOP_MSG_REMOTE_PALETTE, &pal, sizeof(pal));
        }
    }

    int w = s->w;
    int h = s->h;

    // A burst of hundreds of ~1KB tiles can overflow the socket buffer, and
    // coopnet_send_message() DROPS a message it can't get out in time -- a
    // dropped tile then stays wrong on the client for good if the shadow was
    // already updated (the garbled patches seen in testing). So: the shadow is
    // only updated for tiles that were really sent, and at most
    // kMaxTilesPerTick go out per frame; whatever's left differs from the
    // shadow and simply goes out next frame.
    const int kMaxTilesPerTick = 100;
    int sent = 0;
    bool anyLeft = false;
    g_coopSendDroppable = true;
    for (int ty = 0; ty * kCoopRemoteTile < h && !(anyLeft && sent == 0); ty++) {
        for (int tx = 0; tx * kCoopRemoteTile < w; tx++) {
            CoopRemoteTile tile;
            tile.tx = static_cast<int16_t>(tx);
            tile.ty = static_cast<int16_t>(ty);
            memset(tile.pixels, 0, sizeof(tile.pixels));
            size_t tileIndex = static_cast<size_t>(ty) * ((w + kCoopRemoteTile - 1) / kCoopRemoteTile) + tx;
            bool changed = tileIndex < g_coopRemoteTileSent.size() && !g_coopRemoteTileSent[tileIndex];
            for (int row = 0; row < kCoopRemoteTile; row++) {
                int y = ty * kCoopRemoteTile + row;
                if (y >= h) {
                    break;
                }
                int x0 = tx * kCoopRemoteTile;
                int count = w - x0 < kCoopRemoteTile ? w - x0 : kCoopRemoteTile;
                const uint8_t* src = static_cast<const uint8_t*>(s->pixels) + y * s->pitch + x0;
                const uint8_t* shadow = &g_coopRemoteShadow[static_cast<size_t>(y) * w + x0];
                memcpy(tile.pixels + row * kCoopRemoteTile, src, count);
                if (memcmp(shadow, src, count) != 0) {
                    changed = true;
                }
            }
            if (!changed) {
                continue;
            }
            if (sent >= kMaxTilesPerTick) {
                anyLeft = true;
                continue;
            }
            if (coopnet_send_message(g_coopPeerSocket, COOP_MSG_REMOTE_TILE, &tile, sizeof(tile))) {
                sent++;
                if (tileIndex < g_coopRemoteTileSent.size()) {
                    g_coopRemoteTileSent[tileIndex] = 1;
                }
                for (int row = 0; row < kCoopRemoteTile; row++) {
                    int y = ty * kCoopRemoteTile + row;
                    if (y >= h) {
                        break;
                    }
                    int x0 = tx * kCoopRemoteTile;
                    int count = w - x0 < kCoopRemoteTile ? w - x0 : kCoopRemoteTile;
                    memcpy(&g_coopRemoteShadow[static_cast<size_t>(y) * w + x0], tile.pixels + row * kCoopRemoteTile, count);
                }
            } else {
                anyLeft = true;
            }
        }
    }

    g_coopSendDroppable = false;
    (void)anyLeft;
}

// Host: apply the driver's forwarded input. Mouse is absolute, the engine wants
// deltas; a key is a raw scancode press/release.
// Input is QUEUED and applied one event per presented frame: a click's button
// down and up often arrive in the same network batch, and applying both in a
// single frame made mouse_simulate_input() overwrite the down with the up --
// the click vanished ("I can't push the talk button", confirmed via testing).
static void coopnet_remote_host_apply_input(const CoopRemoteInput& in)
{
    if (!g_coopRemoteHostActive || g_coopRemoteViewOnly) {
        return;
    }
    if (g_coopRemoteInputQueue.size() < 256) {
        g_coopRemoteInputQueue.push_back(in);
    }
}

static void coopnet_remote_host_apply_one_input()
{
    if (g_coopRemoteInputQueue.empty()) {
        return;
    }
    CoopRemoteInput in = g_coopRemoteInputQueue.front();
    g_coopRemoteInputQueue.erase(g_coopRemoteInputQueue.begin());

    int curX, curY;
    mouse_get_position(&curX, &curY);
    mouse_simulate_input(in.x - curX, in.y - curY, in.buttons);

    if (in.scancode >= 0) {
        KeyboardData data;
        data.key = in.scancode;
        data.down = in.down;
        kb_simulate_key(&data);
    }
}

// Client side.
static bool g_coopRemoteClientActive = false;
static bool g_coopRemoteClientViewOnly = false;
static bool g_coopRemoteClientTravel = false;

// Mouse presses seen by an SDL event watch (which sees every one, unlike the
// per-frame sampling) -- see coopnet_remote_client_tick().
static bool g_coopLatchedLeft = false;
static bool g_coopLatchedRight = false;

static int SDLCALL coopnet_mouse_press_watch(void* userdata, SDL_Event* event)
{
    (void)userdata;
    if (event->type == SDL_MOUSEBUTTONDOWN) {
        if (event->button.button == SDL_BUTTON_LEFT) {
            g_coopLatchedLeft = true;
        } else if (event->button.button == SDL_BUTTON_RIGHT) {
            g_coopLatchedRight = true;
        }
    }
    return 0;
}
// A MAP_TRANSITION that arrived while a host screen was being shown (the host
// loads the destination map while its world-map screen is still up): applied
// as soon as that screen ends.
static bool g_coopPendingTransitionValid = false;
static CoopMapTransition g_coopPendingTransition;
static void coopnet_client_apply_map_transition(const CoopMapTransition& transition);

// Nonzero while a cutscene plays -- see coopnet_movie_begin() in coopnet.h.
static int g_coopMovieDepth = 0;

void coopnet_movie_begin()
{
    g_coopMovieDepth++;
}

void coopnet_movie_end()
{
    if (g_coopMovieDepth > 0) {
        g_coopMovieDepth--;
    }
    if (g_coopMovieDepth == 0 && g_coopPendingTransitionValid && !g_coopRemoteClientActive) {
        g_coopPendingTransitionValid = false;
        coopnet_client_apply_map_transition(g_coopPendingTransition);
    }
}
static int g_coopRemoteW = 0;
static int g_coopRemoteH = 0;
static std::vector<uint8_t> g_coopRemoteFrame;
static uint8_t g_coopRemotePal[256 * 3];
static bool g_coopRemotePalDirty = false;
static SDL_Surface* g_coopRemoteSurface = NULL;
static int g_coopRemoteLastX = -1;
static int g_coopRemoteLastY = -1;
static int g_coopRemoteLastButtons = -1;
static uint32_t g_coopRemoteLastInputMs = 0;
// Every keyboard scancode is forwarded (edge-detected): "tell me about" needs
// real typing, not just a handful of control keys.
const int kCoopRemoteMaxScancode = 232; // through the modifier keys
static bool g_coopRemoteKeyDown[kCoopRemoteMaxScancode] = { false };

bool coopnet_client_remote_active()
{
    return g_coopRemoteClientActive;
}

static void coopnet_remote_client_begin(const CoopRemoteBegin& b)
{
    if (b.width <= 0 || b.height <= 0 || b.width > 2048 || b.height > 2048) {
        return;
    }
    g_coopRemoteW = b.width;
    g_coopRemoteH = b.height;
    g_coopRemoteFrame.assign(static_cast<size_t>(b.width) * b.height, 0);
    memset(g_coopRemotePal, 0, sizeof(g_coopRemotePal));
    g_coopRemotePalDirty = true;
    if (g_coopRemoteSurface != NULL) {
        SDL_FreeSurface(g_coopRemoteSurface);
        g_coopRemoteSurface = NULL;
    }
    g_coopRemoteSurface = SDL_CreateRGBSurface(0, b.width, b.height, 8, 0, 0, 0, 0);
    g_coopRemoteLastX = g_coopRemoteLastY = g_coopRemoteLastButtons = -1;
    memset(g_coopRemoteKeyDown, 0, sizeof(g_coopRemoteKeyDown));
    g_coopRemoteClientActive = g_coopRemoteSurface != NULL;
    g_coopRemoteClientViewOnly = b.viewOnly != 0;
    g_coopRemoteClientTravel = b.travel != 0;
    g_coopLatchedLeft = false;
    g_coopLatchedRight = false;
    SDL_DelEventWatch(coopnet_mouse_press_watch, NULL);
    SDL_AddEventWatch(coopnet_mouse_press_watch, NULL);
    if (g_coopRemoteClientTravel) {
        // The world map has its own music on the host; the client hears it too.
        gsound_background_play_level_music("03WRLDMP", 12);
    }
    debug_printf("\nCoop: remote screen began on client (%dx%d) active=%d viewOnly=%d\n", b.width, b.height, g_coopRemoteClientActive, b.viewOnly);
}

static void coopnet_remote_client_tile(const CoopRemoteTile& t)
{
    if (!g_coopRemoteClientActive) {
        return;
    }
    for (int row = 0; row < kCoopRemoteTile; row++) {
        int y = t.ty * kCoopRemoteTile + row;
        if (y < 0 || y >= g_coopRemoteH) {
            break;
        }
        int x0 = t.tx * kCoopRemoteTile;
        if (x0 < 0 || x0 >= g_coopRemoteW) {
            return;
        }
        int count = g_coopRemoteW - x0 < kCoopRemoteTile ? g_coopRemoteW - x0 : kCoopRemoteTile;
        memcpy(&g_coopRemoteFrame[static_cast<size_t>(y) * g_coopRemoteW + x0], t.pixels + row * kCoopRemoteTile, count);
    }
}

static void coopnet_remote_client_end()
{
    if (!g_coopRemoteClientActive) {
        return;
    }
    g_coopRemoteClientActive = false;
    SDL_DelEventWatch(coopnet_mouse_press_watch, NULL);
    if (g_coopRemoteSurface != NULL) {
        SDL_FreeSurface(g_coopRemoteSurface);
        g_coopRemoteSurface = NULL;
    }

    // The client's own windows (interface bar, message box, ...) were never
    // told the screen was replaced underneath them -- repaint everything, not
    // just the map, or the old UI stays half-drawn until the mouse "paints"
    // it back (confirmed via testing).
    win_refresh_all(&scr_size);
    tile_refresh_display();
    debug_printf("\nCoop: remote screen ended on client\n");

    if (g_coopRemoteClientTravel) {
        g_coopRemoteClientTravel = false;
        PlayCityMapMusic(); // back to the location's own music (the map load that follows sets it again)
    }

    if (g_coopPendingTransitionValid) {
        g_coopPendingTransitionValid = false;
        coopnet_client_apply_map_transition(g_coopPendingTransition);
    }
}

// Client side, once per presented frame while active (hooked from
// renderPresent()): draws the host's screen over the client's own, and
// forwards the driver's mouse and keys to the host.
static void coopnet_remote_client_tick()
{
    if (g_coopRemoteSurface == NULL || gSdlTextureSurface == NULL) {
        return;
    }

    if (g_coopRemotePalDirty) {
        SDL_Color colors[256];
        for (int i = 0; i < 256; i++) {
            colors[i].r = g_coopRemotePal[i * 3];
            colors[i].g = g_coopRemotePal[i * 3 + 1];
            colors[i].b = g_coopRemotePal[i * 3 + 2];
            colors[i].a = 255;
        }
        SDL_SetPaletteColors(g_coopRemoteSurface->format->palette, colors, 0, 256);
        g_coopRemotePalDirty = false;
    }

    for (int y = 0; y < g_coopRemoteH; y++) {
        memcpy(static_cast<uint8_t*>(g_coopRemoteSurface->pixels) + y * g_coopRemoteSurface->pitch,
            &g_coopRemoteFrame[static_cast<size_t>(y) * g_coopRemoteW], g_coopRemoteW);
    }
    SDL_BlitSurface(g_coopRemoteSurface, NULL, gSdlTextureSurface, NULL);

    if (g_coopRemoteClientViewOnly) {
        return; // watching: nothing is forwarded
    }

    // Forward input: mouse position/buttons when they change, plus key edges.
    uint32_t now = coopnet_now_ms();
    int mx, my;
    mouse_get_position(&mx, &my);
    Uint32 sdlButtons = SDL_GetMouseState(NULL, NULL);
    int buttons = ((sdlButtons & SDL_BUTTON_LMASK) != 0 ? 1 : 0) | ((sdlButtons & SDL_BUTTON_RMASK) != 0 ? 2 : 0);

    const Uint8* keys = SDL_GetKeyboardState(NULL);
    for (int i = 4; i < kCoopRemoteMaxScancode; i++) {
        bool down = keys[i] != 0;
        if (down != g_coopRemoteKeyDown[i]) {
            g_coopRemoteKeyDown[i] = down;
            CoopRemoteInput in;
            in.x = mx;
            in.y = my;
            in.buttons = static_cast<uint8_t>(buttons);
            in.scancode = i;
            in.down = down ? 1 : 0;
            coopnet_send_message(g_coopPeerSocket, COOP_MSG_REMOTE_INPUT, &in, sizeof(in));
        }
    }

    // A quick click can begin AND end between two of our samples (the button is
    // already up again when we look) -- the host then never saw it, which is why
    // picking up an item took several tries. The SDL event watch below latched
    // that press; replay it as an explicit down + up pair.
    int latched = (g_coopLatchedLeft ? 1 : 0) | (g_coopLatchedRight ? 2 : 0);
    g_coopLatchedLeft = false;
    g_coopLatchedRight = false;
    int missed = latched & ~buttons & ~(g_coopRemoteLastButtons > 0 ? g_coopRemoteLastButtons : 0);
    if (missed != 0) {
        CoopRemoteInput down;
        down.x = mx;
        down.y = my;
        down.buttons = static_cast<uint8_t>(buttons | missed);
        down.scancode = -1;
        down.down = 0;
        coopnet_send_message(g_coopPeerSocket, COOP_MSG_REMOTE_INPUT, &down, sizeof(down));
        CoopRemoteInput up = down;
        up.buttons = static_cast<uint8_t>(buttons);
        coopnet_send_message(g_coopPeerSocket, COOP_MSG_REMOTE_INPUT, &up, sizeof(up));
        g_coopRemoteLastX = mx;
        g_coopRemoteLastY = my;
        g_coopRemoteLastButtons = buttons;
        g_coopRemoteLastInputMs = now;
        return;
    }

    if (mx != g_coopRemoteLastX || my != g_coopRemoteLastY || buttons != g_coopRemoteLastButtons) {
        if (buttons != g_coopRemoteLastButtons || now - g_coopRemoteLastInputMs >= 30) {
            g_coopRemoteLastX = mx;
            g_coopRemoteLastY = my;
            g_coopRemoteLastButtons = buttons;
            g_coopRemoteLastInputMs = now;
            CoopRemoteInput in;
            in.x = mx;
            in.y = my;
            in.buttons = static_cast<uint8_t>(buttons);
            in.scancode = -1;
            in.down = 0;
            coopnet_send_message(g_coopPeerSocket, COOP_MSG_REMOTE_INPUT, &in, sizeof(in));
        }
    }
}

// Called by renderPresent() (svga.cc) before every present.
void coopnet_remote_screen_frame_hook()
{
    if (g_coopRemoteHostActive) {
        coopnet_remote_host_tick();
    } else if (g_coopRemoteClientActive) {
        coopnet_remote_client_tick();
    }
}

// Host-side: the client just finished driving a barter -- its companion's
// inventory changed, so refresh what the client sees.
void coopnet_after_client_barter()
{
    if (g_coopRole == CoopRole::Host && g_coopConnState == CoopConnState::Connected && g_coopDialogueDrivenByClient) {
        coopnet_host_broadcast_companion_inventory();
    }
}

// Host-side only: last global-variable values actually sent to the client.
// Kept as a shadow copy so changes can be found by diffing the array -- that
// catches every writer, including the engine's direct game_global_vars[i] = x
// writes (vault water counter, addiction flags) that never go through
// game_set_global_var(). -1 length = nothing sent yet (a fresh connection
// resets it so the whole array goes out again).
static std::vector<int32_t> g_coopGvarShadow;
static bool g_coopGvarShadowValid = false;
static int g_coopGvarSliceCursor = 0;
static uint32_t g_coopGvarLastSliceMs = 0;
const uint32_t kCoopGvarSliceIntervalMs = 1000;

static void coopnet_send_gvar_entries(const CoopGvarEntry* entries, int count)
{
    CoopGvarDelta msg;
    while (count > 0) {
        int n = count > kCoopGvarMaxEntries ? kCoopGvarMaxEntries : count;
        msg.count = static_cast<uint8_t>(n);
        memcpy(msg.entries, entries, n * sizeof(CoopGvarEntry));
        coopnet_send_message(g_coopPeerSocket, COOP_MSG_GVAR_DELTA, &msg, static_cast<uint16_t>(1 + n * sizeof(CoopGvarEntry)));
        entries += n;
        count -= n;
    }
}

// Host-side only, called on the same ~100ms cadence as the position
// broadcast. Sends every global variable that changed since last time, and
// on top of that a rotating slice of the array every second regardless of
// changes -- so a value the client's own independent simulation happened to
// overwrite (or a dropped message) heals itself within a few seconds, instead
// of leaving the two worlds quietly disagreeing about a quest forever.
static void coopnet_host_broadcast_gvars()
{
    if (game_global_vars == NULL || num_game_global_vars <= 0) {
        return;
    }

    if (!g_coopGvarShadowValid || static_cast<int>(g_coopGvarShadow.size()) != num_game_global_vars) {
        g_coopGvarShadow.assign(num_game_global_vars, 0);
        g_coopGvarShadowValid = true;
        // Force the first pass to send everything, including zeros.
        std::vector<CoopGvarEntry> all;
        all.reserve(num_game_global_vars);
        for (int i = 0; i < num_game_global_vars; i++) {
            CoopGvarEntry e = { i, game_global_vars[i] };
            all.push_back(e);
            g_coopGvarShadow[i] = game_global_vars[i];
        }
        coopnet_send_gvar_entries(all.data(), static_cast<int>(all.size()));
        debug_printf("\nCoop: sent full global variable snapshot (%d vars)\n", num_game_global_vars);
        return;
    }

    std::vector<CoopGvarEntry> changed;
    for (int i = 0; i < num_game_global_vars; i++) {
        if (g_coopGvarShadow[i] != game_global_vars[i]) {
            CoopGvarEntry e = { i, game_global_vars[i] };
            changed.push_back(e);
            g_coopGvarShadow[i] = game_global_vars[i];
        }
    }
    if (!changed.empty()) {
        coopnet_send_gvar_entries(changed.data(), static_cast<int>(changed.size()));
        debug_printf("\nCoop: sent %d changed global variable(s)\n", static_cast<int>(changed.size()));
    }

    uint32_t now = coopnet_now_ms();
    if (now - g_coopGvarLastSliceMs >= kCoopGvarSliceIntervalMs) {
        g_coopGvarLastSliceMs = now;
        std::vector<CoopGvarEntry> slice;
        for (int k = 0; k < kCoopGvarMaxEntries && k < num_game_global_vars; k++) {
            int i = (g_coopGvarSliceCursor + k) % num_game_global_vars;
            CoopGvarEntry e = { i, game_global_vars[i] };
            slice.push_back(e);
        }
        g_coopGvarSliceCursor = (g_coopGvarSliceCursor + kCoopGvarMaxEntries) % num_game_global_vars;
        coopnet_send_gvar_entries(slice.data(), static_cast<int>(slice.size()));
    }
}

static void coopnet_host_broadcast_positions()
{
    if (g_coopCompanion != NULL) {
        CoopPosition pos;
        pos.which = 0;
        pos.tile = g_coopCompanion->tile;
        pos.elevation = g_coopCompanion->elevation;
        pos.rotation = g_coopCompanion->rotation;
        pos.hp = stat_level(g_coopCompanion, STAT_CURRENT_HIT_POINTS);
        pos.fidBase = g_coopCompanion->fid & 0xFFF;
        pos.weaponCode = (g_coopCompanion->fid & 0xF000) >> 12;
        pos.transFlags = g_coopCompanion->flags & OBJECT_FLAG_0xFC000;
        coopnet_send_message(g_coopPeerSocket, COOP_MSG_POSITION, &pos, sizeof(pos));
    }

    if (obj_dude != NULL) {
        CoopPosition pos;
        pos.which = 1;
        pos.tile = obj_dude->tile;
        pos.elevation = obj_dude->elevation;
        pos.rotation = obj_dude->rotation;
        pos.hp = stat_level(obj_dude, STAT_CURRENT_HIT_POINTS);
        pos.fidBase = obj_dude->fid & 0xFFF;
        pos.weaponCode = (obj_dude->fid & 0xF000) >> 12;
        pos.transFlags = obj_dude->flags & OBJECT_FLAG_0xFC000;
        coopnet_send_message(g_coopPeerSocket, COOP_MSG_POSITION, &pos, sizeof(pos));
    }
}

// Host-side only, called alongside coopnet_host_broadcast_positions() at the
// same cadence: keeps the client's independent game clock from drifting off
// the host's (see CoopGameTime's comment). Diffed against the last value
// actually sent so this is cheap to call unconditionally every broadcast
// tick -- game_time() only actually changes a few times a second at most
// during normal play, and in large jumps during worldmap travel.
static void coopnet_host_broadcast_game_time()
{
    static int32_t lastSentGameTime = -1;

    int32_t current = static_cast<int32_t>(game_time());
    if (current == lastSentGameTime) {
        return;
    }
    lastSentGameTime = current;

    CoopGameTime msg;
    msg.gameTime = current;
    coopnet_send_message(g_coopPeerSocket, COOP_MSG_GAME_TIME, &msg, sizeof(msg));
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
        // pid 0x1000000 is the player character's prototype: only ever obj_dude
        // or the companion (never a streamed NPC) -- see coopnet_host_broadcast_world().
        if (critter == NULL || critter == obj_dude || critter == g_coopCompanion || (critter->pid == 0x1000000 || critter->pid == kCoopCompanionPid)) {
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
        participant.fid = critter->fid;
        participant.resync = 0;
        coopnet_send_message(g_coopPeerSocket, COOP_MSG_COMBAT_PARTICIPANT, &participant, sizeof(participant));
    }
}

// Host-side only: WORLD SYNC. The client keeps its own copy of the map, so
// every NPC there used to live its own life (wandering, and script-spawned NPCs
// existing only on the host). Streams every living critter near either player
// as a "participant" (same message/apply path as combat), matched on the client
// by (Object::id, pid) -- ids of map-loaded critters come from the map file, so
// both worlds agree -- else nearest same-pid critter, else spawned. Only changes
// are sent, plus a keep-alive refresh every few seconds; dead critters are
// sent only if they were alive when last sent (so the client plays the death).
struct CoopWorldShadow {
    int32_t id;
    int32_t pid;
    int32_t tile;
    int32_t rotation;
    int32_t hp;
    uint8_t dead;
    uint32_t lastSentMs;
    bool seen; // found by the current scan
    uint32_t lastCheckMs; // last existence check while unseen
};

// Ground items are streamed too (an encounter map's dead body, items dropped
// or removed by scripts): the client purges its own ground items on a map load
// and receives the host's, so the two worlds hold the same loose objects.
struct CoopWorldItemShadow {
    int32_t pid;
    int32_t tile;
    int32_t elevation;
    bool seen;
    uint32_t lastSentMs;
};
static std::vector<CoopWorldItemShadow> g_coopWorldItemShadow;
static std::vector<CoopWorldShadow> g_coopWorldShadow;
static uint32_t g_coopWorldLastMs = 0;
static Object* g_coopWorldStampDude = NULL;
static int g_coopWorldStampMap = -2;
const uint32_t kCoopWorldIntervalMs = 100;
const uint32_t kCoopWorldRefreshMs = 1500;
const int kCoopWorldRadius = 45;
const int kCoopWorldMaxPerTick = 40;

static void coopnet_host_reset_world_shadow()
{
    g_coopWorldShadow.clear();
    g_coopWorldItemShadow.clear();
}

// Does a visible critter with this id/pid still exist anywhere on the host's map?
static bool coopnet_host_critter_exists(int32_t id, int32_t pid)
{
    for (int elevation = 0; elevation < ELEVATION_COUNT; elevation++) {
        for (Object* o = obj_find_first_at(elevation); o != NULL; o = obj_find_next_at()) {
            if (o->id == id && o->pid == pid && FID_TYPE(o->fid) == OBJ_TYPE_CRITTER && (o->flags & OBJECT_HIDDEN) == 0 && o->tile != -1) {
                return true;
            }
        }
    }
    return false;
}

static void coopnet_host_broadcast_world()
{
    if (obj_dude == NULL) {
        return;
    }

    uint32_t now = coopnet_now_ms();
    if (now - g_coopWorldLastMs < kCoopWorldIntervalMs) {
        return;
    }
    g_coopWorldLastMs = now;

    int mapIndex = map_get_index_number();
    if (g_coopWorldStampDude != obj_dude || g_coopWorldStampMap != mapIndex) {
        g_coopWorldStampDude = obj_dude;
        g_coopWorldStampMap = mapIndex;
        g_coopWorldShadow.clear();
    }

    bool combat = isInCombat();
    int sent = 0;
    bool capHit = false;

    for (size_t i = 0; i < g_coopWorldShadow.size(); i++) {
        g_coopWorldShadow[i].seen = false;
    }
    for (size_t i = 0; i < g_coopWorldItemShadow.size(); i++) {
        g_coopWorldItemShadow[i].seen = false;
    }
    std::vector<CoopWorldItemShadow> itemsNow;

    Object* anchors[2] = { obj_dude, g_coopCompanion };
    for (int a = 0; a < 2; a++) {
        Object* anchor = anchors[a];
        // The client's character (the companion) is an anchor too, also on the
        // host's own elevation: it used to be skipped there, so everything
        // around a client who had wandered away from the host was never sent
        // and only appeared once the host walked up to it.
        if (anchor == NULL) {
            continue;
        }

        for (Object* critter = obj_find_first_at(anchor->elevation); critter != NULL; critter = obj_find_next_at()) {
            if (FID_TYPE(critter->fid) == OBJ_TYPE_ITEM) {
                if ((critter->flags & OBJECT_HIDDEN) == 0 && critter->tile != -1 && itemsNow.size() < 400
                    && tile_dist(critter->tile, anchor->tile) <= kCoopWorldRadius) {
                    bool alreadyListed = false;
                    for (size_t n = 0; n < itemsNow.size(); n++) {
                        if (itemsNow[n].pid == critter->pid && itemsNow[n].tile == critter->tile && itemsNow[n].elevation == critter->elevation) {
                            alreadyListed = true;
                            break;
                        }
                    }
                    if (alreadyListed) {
                        continue;
                    }
                    CoopWorldItemShadow key;
                    key.pid = critter->pid;
                    key.tile = critter->tile;
                    key.elevation = critter->elevation;
                    key.seen = true;
                    key.lastSentMs = 0;
                    itemsNow.push_back(key);
                }
                continue;
            }
            // The player prototype (pid 0x1000000) is the host character or the
            // client's companion, never a world NPC. During barter/loot the host
            // temporarily swaps obj_dude for the companion, which made the real
            // host character look like an NPC here -- the client then spawned a
            // "copy of the companion" that vanished a moment later.
            if (FID_TYPE(critter->fid) != OBJ_TYPE_CRITTER || critter == obj_dude || critter == g_coopCompanion || (critter->pid == 0x1000000 || critter->pid == kCoopCompanionPid)) {
                continue;
            }
            if ((critter->flags & OBJECT_HIDDEN) != 0 || critter->tile == -1) {
                continue;
            }
            if (tile_dist(critter->tile, anchor->tile) > kCoopWorldRadius) {
                continue;
            }

            if (combat) {
                bool inList = false;
                int listCount = combat_get_list_count();
                for (int i = 0; i < listCount; i++) {
                    if (combat_get_list_item(i) == critter) {
                        inList = true;
                        break;
                    }
                }
                if (inList) {
                    continue; // already covered by the faster combat broadcast
                }
            }

            bool dead = critter_is_dead(critter);
            int32_t hp = stat_level(critter, STAT_CURRENT_HIT_POINTS);

            CoopWorldShadow* shadow = NULL;
            for (size_t i = 0; i < g_coopWorldShadow.size(); i++) {
                if (g_coopWorldShadow[i].id == critter->id && g_coopWorldShadow[i].pid == critter->pid) {
                    shadow = &g_coopWorldShadow[i];
                    break;
                }
            }

            // A corpse is sent once (dead=true in the shadow afterwards), whether
            // or not it was seen alive: after the client purges its own copy of
            // the map, corpses have to be streamed too.
            if (shadow != NULL) {
                shadow->seen = true;
            }
            if (dead && shadow != NULL && shadow->dead) {
                continue;
            }

            bool changed = shadow == NULL
                || shadow->tile != critter->tile
                || shadow->rotation != critter->rotation
                || shadow->hp != hp
                || (shadow->dead != 0) != dead
                || now - shadow->lastSentMs >= kCoopWorldRefreshMs;
            if (!changed) {
                continue;
            }
            if (sent >= kCoopWorldMaxPerTick) {
                capHit = true;
                break;
            }

            CoopCombatParticipant participant;
            participant.id = critter->id;
            participant.pid = critter->pid;
            participant.tile = critter->tile;
            participant.elevation = critter->elevation;
            participant.rotation = critter->rotation;
            participant.hp = hp;
            participant.isDead = dead ? 1 : 0;
            participant.fid = critter->fid;
            participant.resync = (shadow != NULL && shadow->tile == critter->tile && shadow->hp == hp && (shadow->dead != 0) == dead) ? 1 : 0;
            if (!coopnet_send_message(g_coopPeerSocket, COOP_MSG_COMBAT_PARTICIPANT, &participant, sizeof(participant))) {
                continue;
            }
            sent++;

            if (shadow == NULL) {
                CoopWorldShadow fresh;
                memset(&fresh, 0, sizeof(fresh));
                fresh.id = critter->id;
                fresh.pid = critter->pid;
                fresh.seen = true;
                g_coopWorldShadow.push_back(fresh);
                shadow = &g_coopWorldShadow.back();
            }
            shadow->tile = critter->tile;
            shadow->rotation = critter->rotation;
            shadow->hp = hp;
            shadow->dead = dead ? 1 : 0;
            shadow->lastSentMs = now;
        }
        if (capHit) {
            break;
        }
    }

    // Critters the client mirrors that the host no longer has (destroyed, left
    // the map, hidden by a script): tell the client to drop its copy. Only
    // after a complete scan, and only after an exact existence check -- an NPC
    // that merely walked out of the streaming radius is NOT removed.
    if (!capHit) {
        int checks = 0;
        for (size_t i = 0; i < g_coopWorldShadow.size() && checks < 10;) {
            CoopWorldShadow& s = g_coopWorldShadow[i];
            if (s.seen || now - s.lastCheckMs < 2000) {
                i++;
                continue;
            }
            s.lastCheckMs = now;
            checks++;
            if (coopnet_host_critter_exists(s.id, s.pid)) {
                i++;
                continue;
            }
            CoopWorldRemove rem;
            rem.id = s.id;
            rem.pid = s.pid;
            if (coopnet_send_message(g_coopPeerSocket, COOP_MSG_WORLD_REMOVE, &rem, sizeof(rem))) {
                g_coopWorldShadow.erase(g_coopWorldShadow.begin() + i);
            } else {
                i++;
            }
        }
    }

    // Ground items: same idea, keyed by (pid, tile, elevation).
    int itemSends = 0;
    for (size_t n = 0; n < itemsNow.size(); n++) {
        CoopWorldItemShadow* known = NULL;
        for (size_t i = 0; i < g_coopWorldItemShadow.size(); i++) {
            CoopWorldItemShadow& s = g_coopWorldItemShadow[i];
            if (s.pid == itemsNow[n].pid && s.tile == itemsNow[n].tile && s.elevation == itemsNow[n].elevation) {
                known = &s;
                break;
            }
        }
        if (known != NULL) {
            known->seen = true;
            if (now - known->lastSentMs < 10000) {
                continue;
            }
        }
        if (itemSends >= kCoopWorldMaxPerTick) {
            continue;
        }
        CoopWorldItem msg;
        msg.pid = itemsNow[n].pid;
        msg.tile = itemsNow[n].tile;
        msg.elevation = itemsNow[n].elevation;
        msg.present = 1;
        if (!coopnet_send_message(g_coopPeerSocket, COOP_MSG_WORLD_ITEM, &msg, sizeof(msg))) {
            continue;
        }
        itemSends++;
        if (known == NULL) {
            itemsNow[n].lastSentMs = now;
            g_coopWorldItemShadow.push_back(itemsNow[n]);
        } else {
            known->lastSentMs = now;
        }
    }
    // Items the client has but the host no longer does (picked up, destroyed).
    for (size_t i = 0; i < g_coopWorldItemShadow.size();) {
        CoopWorldItemShadow& s = g_coopWorldItemShadow[i];
        bool inRadius = false;
        for (int a = 0; a < 2; a++) {
            if (anchors[a] != NULL && anchors[a]->elevation == s.elevation && tile_dist(s.tile, anchors[a]->tile) <= kCoopWorldRadius) {
                inRadius = true;
            }
        }
        if (s.seen || !inRadius || itemsNow.size() >= 400) {
            i++;
            continue;
        }
        CoopWorldItem msg;
        msg.pid = s.pid;
        msg.tile = s.tile;
        msg.elevation = s.elevation;
        msg.present = 0;
        if (coopnet_send_message(g_coopPeerSocket, COOP_MSG_WORLD_ITEM, &msg, sizeof(msg))) {
            g_coopWorldItemShadow.erase(g_coopWorldItemShadow.begin() + i);
        } else {
            i++;
        }
    }
}

// Host-side only: sends a full snapshot of the companion's current inventory
// -- see the CoopInventorySync comment above for why this exists and why
// it's a full snapshot rather than a diff.
// Which hand the companion is wielding with (interface item slot). The host's
// value is authoritative; the client updates it via INVENTORY_PUSH whenever it
// switches hands or closes its inventory.
static uint8_t g_coopCompanionActiveHand = 1;

static void coopnet_build_inventory_snapshot(Object* critter, CoopInventorySync& sync)
{
    memset(&sync, 0, sizeof(sync));
    sync.activeHand = g_coopCompanionActiveHand;
    Inventory* inventory = &(critter->data.inventory);

    int count = inventory->length;
    if (count > kCoopMaxInventorySyncItems) {
        debug_printf("\nCoop: companion inventory has %d items, only syncing first %d\n", count, kCoopMaxInventorySyncItems);
        count = kCoopMaxInventorySyncItems;
    }

    sync.itemCount = static_cast<uint8_t>(count);
    for (int i = 0; i < count; i++) {
        Object* item = inventory->items[i].item;
        sync.items[i].pid = item->pid;
        sync.items[i].quantity = inventory->items[i].quantity;
        sync.items[i].flags = item->flags & OBJECT_EQUIPPED;
        sync.items[i].dataA = item->data.item.weapon.ammoQuantity;
        sync.items[i].dataB = item_get_type(item) == ITEM_TYPE_WEAPON ? item->data.item.weapon.ammoTypePid : 0;
    }
}

// Recomputes a critter's look (worn armor -> body art, wielded weapon ->
// animation set) from what it actually has equipped. Same logic as
// inventry.cc's adjust_fid(), which only ever ran for obj_dude.
static void coopnet_refresh_critter_fid(Object* critter)
{
    if (critter == NULL || FID_TYPE(critter->fid) != OBJ_TYPE_CRITTER || (critter->data.critter.combat.results & DAM_DEAD) != 0) {
        return;
    }

    int base = art_vault_guy_num;
    Object* worn = inven_worn(critter);
    Proto* proto;
    if (worn != NULL && proto_ptr(worn->pid, &proto) != -1) {
        int v = stat_level(critter, STAT_GENDER) == GENDER_FEMALE ? proto->item.data.armor.femaleFid : proto->item.data.armor.maleFid;
        if (v != -1) {
            base = v;
        }
    }

    Object* held = g_coopCompanionActiveHand != 0 ? inven_right_hand(critter) : inven_left_hand(critter);
    int animationCode = 0;
    if (held != NULL && proto_ptr(held->pid, &proto) != -1 && proto->item.type == ITEM_TYPE_WEAPON) {
        animationCode = proto->item.data.weapon.animationCode;
    }

    int newFid = art_id(OBJ_TYPE_CRITTER, base, FID_ANIM_TYPE(critter->fid), animationCode, (critter->fid & 0x70000000) >> 28);
    if (newFid != critter->fid && art_exists(newFid)) {
        Rect rect;
        obj_change_fid(critter, newFid, &rect);
        tile_refresh_rect(&rect, critter->elevation);
    }
}

static void coopnet_host_broadcast_companion_inventory()
{
    if (g_coopCompanion == NULL) {
        return;
    }

    CoopInventorySync sync;
    coopnet_build_inventory_snapshot(g_coopCompanion, sync);
    int count = sync.itemCount;

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
static int g_coopHostMapLoadDepth = 0;

bool coopnet_client_map_loading()
{
    return g_coopClientMapLoading;
}

void coopnet_host_map_load_enter()
{
    g_coopHostMapLoadDepth++;
}

void coopnet_host_map_load_leave()
{
    if (g_coopHostMapLoadDepth > 0) {
        g_coopHostMapLoadDepth--;
    }
}

static void coopnet_host_check_map_transition()
{
    if (g_coopHostMapLoadDepth > 0 || g_coopMovieDepth > 0) {
        return;
    }

    bool mapChanged = strncmp(g_coopHostLastMapName, map_data.name, sizeof(g_coopHostLastMapName)) != 0;

    // A door/stairs leading into a small building is often modeled as an
    // elevation change on the SAME map file rather than a real map
    // transition -- map_data.name never changes, so the check above alone
    // missed it entirely. See g_coopHostLastElevation's comment.
    bool elevationChanged = !mapChanged && g_coopHostLastElevation != -1 && g_coopHostLastElevation != obj_dude->elevation;

    if (!mapChanged && !elevationChanged) {
        return;
    }

    strncpy(g_coopHostLastMapName, map_data.name, sizeof(g_coopHostLastMapName) - 1);
    g_coopHostLastMapName[sizeof(g_coopHostLastMapName) - 1] = '\0';
    g_coopHostLastElevation = obj_dude->elevation;

    if (mapChanged && map_data.name[0] == '\0') {
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

    bool resyncOnly = g_coopHostResyncOnly;
    g_coopHostResyncOnly = false;

    if (mapChanged && resyncOnly && g_coopCompanion != NULL) {
        // The client only reloaded a save of its own: the host's map and its
        // companion are untouched. Destroying the companion here (mid-fight it
        // is in the combat list and has its turn running) broke the whole
        // combat -- just tell the client where everything is.
        debug_printf("\nCoop: client resync on %.16s, keeping the host's companion as it is\n", map_data.name);
    } else if (mapChanged) {
        debug_printf("\nCoop: host map changed to %.16s, respawning companion (old companion=%p pid=%d tile=%d)\n",
            map_data.name, (void*)g_coopCompanion, g_coopCompanion != NULL ? g_coopCompanion->pid : -1, g_coopCompanion != NULL ? g_coopCompanion->tile : -1);

        g_coopCompanion = coopnet_respawn_companion(g_coopCompanion, obj_dude->pid, obj_dude->tile, obj_dude->elevation);

        debug_printf("\nCoop-debug: new companion=%p pid=%d tile=%d\n",
            (void*)g_coopCompanion, g_coopCompanion != NULL ? g_coopCompanion->pid : -1, g_coopCompanion != NULL ? g_coopCompanion->tile : -1);

        g_coopLastCommandedTile[0] = -1;
        g_coopLastCommandedTile[1] = -1;
    } else {
        // Elevation-only: the map is already loaded on both sides and the
        // companion object is still perfectly valid (it never got
        // destroyed), so none of the heavy map-change bookkeeping above
        // applies -- just tell the client which elevation to move to.
        debug_printf("\nCoop: host elevation changed to %d on map %.16s (same map)\n", obj_dude->elevation, map_data.name);
    }

    if (g_coopConnState != CoopConnState::Connected) {
        return;
    }

    CoopMapTransition transition;
    memset(&transition, 0, sizeof(transition));
    strncpy(transition.mapName, map_data.name, sizeof(transition.mapName) - 1);
    transition.tile = obj_dude->tile;
    transition.elevation = obj_dude->elevation;
    transition.rotation = obj_dude->rotation;
    transition.sameMapElevationOnly = mapChanged ? 0 : 1;

    bool sent = coopnet_send_message(g_coopPeerSocket, COOP_MSG_MAP_TRANSITION, &transition, sizeof(transition));
    debug_printf("\nCoop: sent MAP_TRANSITION to %.16s (tile=%d elevation=%d sameMapElevationOnly=%d) success=%d\n",
        transition.mapName, transition.tile, transition.elevation, transition.sameMapElevationOnly, sent);

    if (mapChanged && kCoopCompanionInventorySyncEnabled) {
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
        coopnet_report_glitch("obj_pid_new failed applying item drop (pid=%d)\n", evt.pid);
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

// Client-side only: applies one COOP_MSG_SCENERY_STATE. Finds the local
// scenery object at the same (pid, tile, elevation) the host reported and
// drives it through the real obj_open()/obj_close() (protinst.cc) --
// idempotent (no-op if already in the target state) and plays the same
// real animation a local interactive use would, rather than a raw snap.
// obj_unlock() first on the "opening" side: the host's own lock/key/
// lockpick state isn't synced at all, and obj_toggle_open() (which
// obj_open() calls) refuses to act on a door it still thinks is locked --
// the host only ever reports "opened" once it actually succeeded, so the
// client unlocking to match is always correct, never a shortcut.
static void coopnet_apply_scenery_state(const CoopSceneryState& state)
{
    for (Object* object = obj_find_first_at(state.elevation); object != NULL; object = obj_find_next_at()) {
        if (object->tile == state.tile && object->pid == state.pid) {
            if (state.isOpen) {
                obj_unlock(object);
                obj_open(object);
            } else {
                obj_close(object);
            }
            return;
        }
    }

    coopnet_report_glitch("scenery state notification had no matching local object (pid=%d, tile=%d)\n", state.pid, state.tile);
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

    coopnet_report_glitch("item pickup notification had no matching ground item (pid=%d, tile=%d)\n", evt.pid, evt.tile);
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
            coopnet_report_glitch("obj_pid_new failed applying companion inventory sync (pid=%d)\n", sync.items[i].pid);
            continue;
        }
        newItem->data.item.weapon.ammoQuantity = sync.items[i].dataA;
        if (item_get_type(newItem) == ITEM_TYPE_WEAPON) {
            newItem->data.item.weapon.ammoTypePid = sync.items[i].dataB;
        }
        newItem->flags |= (sync.items[i].flags & OBJECT_EQUIPPED);
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

// Host-side only, called from scripts.cc's SCRIPT_REQUEST_STEALING drain
// instead of the normal inven_steal_container() when the stealer is the
// companion. The real steal flow (inven_steal_container() -> loot_container())
// pops a blocking modal loot window and sits in its own get_input() loop
// waiting for a human to browse the target's inventory and click an item --
// that's the HOST's screen/input, for an action the CLIENT's player asked
// for, so there's nobody who could actually drive it. Auto-resolves instead:
// picks one item from the target's inventory at random and rolls the same
// skill_check_stealing() the real UI path uses underneath -- identical odds
// (skill level, item size, facing, target's own Steal skill for the catch
// roll), and it prints the same "You steal the %s."/"You're caught stealing
// the %s." message. Also replicates loot_container()'s two side effects that
// live outside skill_check_stealing() itself, since our caller (scripts.cc)
// skips that function entirely:
//   - success: grants the same flat 10 XP a real single-item steal earns
//     (loot_container()'s stealingXp/stealingXpBonus bookkeeping, but for
//     exactly one item instead of a whole browsing session).
//   - caught: fires SCRIPT_PROC_PICKUP on the target's own script, same as
//     loot_container()'s isCaughtStealing path -- this is the hook vanilla
//     NPC scripts use to turn hostile on the thief, so a caught companion
//     steal can still start a fight, same as a caught player steal would.
bool coopnet_auto_resolve_companion_steal(Object* stealer, Object* target)
{
    if (stealer == NULL || target == NULL || stealer == target) {
        return false;
    }

    Inventory* inventory = &(target->data.inventory);
    if (inventory->length == 0) {
        return false;
    }

    int index = roll_random(0, inventory->length - 1);
    Object* item = inventory->items[index].item;

    // Each companion steal attempt is its own one-shot try, not part of a
    // multi-item UI session -- reset the same accumulating penalty counter
    // inven_steal_container() resets around a real (human-driven) session,
    // so a stale nonzero value left over from the player's own last steal
    // doesn't unfairly penalize the companion's.
    gStealCount = 0;

    int rc = skill_check_stealing(stealer, target, item, false);
    if (rc != 1) {
        // Caught -- same consequence hook loot_container() fires: let the
        // target's own script react (typically turning hostile).
        int sid;
        if (obj_sid(target, &sid) != -1) {
            scr_set_objs(sid, stealer, NULL);
            exec_script_proc(sid, SCRIPT_PROC_PICKUP);
        }
        return false;
    }

    item_move_force(target, stealer, item, 1);

    if (!isPartyMember(target)) {
        int xp = 300 - skill_level(stealer, SKILL_STEAL);
        if (xp > 10) {
            xp = 10;
        }
        if (xp > 0) {
            stat_pc_add_experience(xp);
        }
    }

    return true;
}

// Host-side only: enqueues a client's action request (pickup, use, or skill)
// rather than applying it immediately -- see the g_coopActionQueue comment
// above for why. `skill` is only meaningful for COOP_COMPANION_ACTION_SKILL.
static void coopnet_enqueue_companion_action(CoopCompanionActionKind kind, const CoopItemEvent& evt, int32_t skill = -1, bool targetIsHostDude = false)
{
    // In a fight these only make sense on the companion's own turn (they cost
    // its action points). Refuse out of turn and say so, rather than queueing
    // something that would run at a random later moment -- or never.
    if (isInCombat() && !g_coopHostCombatTurnActive) {
        CoopCombatText msg;
        memset(&msg, 0, sizeof(msg));
        snprintf(msg.text, sizeof(msg.text), "It isn't your turn yet.");
        coopnet_send_message(g_coopPeerSocket, COOP_MSG_COMBAT_TEXT, &msg, sizeof(msg));
        return;
    }

    if (g_coopActionQueueLen >= kCoopActionQueueCapacity) {
        coopnet_report_glitch("companion action queue full, dropping request (kind=%d, pid=%d, tile=%d)\n", kind, evt.pid, evt.tile);
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
// Host: the client went away (closed its game, connection dropped). Give the
// host its game back: any screen the client was driving (dialogue, barter,
// loot, world map) returns to the host's own mouse/keyboard, and say so.
static void coopnet_host_on_client_lost()
{
    g_coopDialogueDrivenByClient = false;
    g_coopDialogueDriverPending = false;
    g_coopDialogueHostInitiated = false;
    g_coopPendingDialoguePick = -1;
    g_coopRemoteHostActive = false;
    g_coopRemoteForceDrive = false;
    g_coopRemoteViewOnly = false;
    g_coopRemoteInputQueue.clear();
    g_coopHostCombatTurnActive = false;
    g_coopTravelClientDrives = false;
    g_coopCompanionActionBusy = false;
    g_coopActionQueueLen = 0;
    g_coopActionQueueHead = 0;
    coopnet_host_reset_world_shadow();

    // Keep the client's character safe the moment it drops out.
    g_coopLastProfileAutosaveMs = 0;
    coopnet_host_autosave_profile();

    char message[40];
    strcpy(message, "Client left the game.");
    display_print(message);
    debug_printf("\nCoop: client lost -- host control restored\n");
}

static void coopnet_host_process_action_queue()
{
    // In a fight the companion can do these only on its OWN turn, and only
    // once whatever it was animating (a move, an attack) has finished -- like
    // a player who can't click while something is still playing. They used to
    // be held back for the entire turn (the turn loop never drained the
    // queue) and run, with no action points, during the enemies' turns:
    // either way, in combat the client could not pick anything up, open a door
    // or search a body.
    if (isInCombat()) {
        if (!g_coopHostCombatTurnActive) {
            return;
        }
        if (g_coopCompanion != NULL && anim_busy(g_coopCompanion)) {
            return;
        }
    }

    if (g_coopCompanionActionBusy) {
        // A use (a door, a lever) has no completion hook, so "done" is: the
        // companion has stopped animating. The long timeout is only a backstop.
        uint32_t elapsed = coopnet_now_ms() - g_coopCompanionActionStartMs;
        bool finished = elapsed > 500 && (g_coopCompanion == NULL || !anim_busy(g_coopCompanion));
        if (finished) {
            g_coopCompanionActionBusy = false;
        } else if (elapsed > kCoopCompanionActionTimeoutMs) {
            debug_printf("\nCoop: companion action still busy after %ums, giving up waiting on it\n", elapsed);
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
            coopnet_report_glitch("action_use_skill_on failed (skill=%d, target=obj_dude)\n", request.skill);
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
    } else if (request.kind == COOP_COMPANION_ACTION_LOOT) {
        wantType = OBJ_TYPE_CRITTER;

        // Corpse positions can differ a little between the two worlds: take
        // the exact tile if there is one, else the nearest same-pid critter.
        Object* corpse = NULL;
        int bestDist = 8;
        for (Object* object = obj_find_first_at(evt.elevation); object != NULL; object = obj_find_next_at()) {
            if (object->pid != evt.pid || FID_TYPE(object->fid) != OBJ_TYPE_CRITTER || object == obj_dude || object == g_coopCompanion) {
                continue;
            }
            int dist = tile_dist(object->tile, evt.tile);
            if (dist < bestDist) {
                bestDist = dist;
                corpse = object;
            }
        }
        if (corpse != NULL) {
            debug_printf("\nCoop: companion loots critter at tile=%d\n", corpse->tile);
            g_coopCompanionActionBusy = true;
            g_coopCompanionActionStartMs = coopnet_now_ms();
            if (action_loot_container(g_coopCompanion, corpse) == -1) {
                g_coopCompanionActionBusy = false;
            }
        } else {
            debug_printf("\nCoop: LOOT_REQUEST found no critter (pid=%d tile=%d)\n", evt.pid, evt.tile);
        }
        return;
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
                // Consumed by whichever screen this use opens next (currently
                // only elevator_select() checks it) -- harmlessly expires
                // unused for every other kind of "use" (doors, etc.).
                coopnet_note_companion_used_object();
                int useRc = action_use_an_object(g_coopCompanion, object);
                int useSid = -1;
                obj_sid(object, &useSid);
                Proto* useProto = NULL;
                proto_ptr(object->pid, &useProto);
                debug_printf("\nCoop-debug: companion use of pid=%d -> action rc=%d, scenery type=%d, script sid=%d\n",
                    object->pid, useRc, useProto != NULL ? useProto->scenery.type : -1, useSid);
            } else {
                if (action_use_skill_on(g_coopCompanion, object, request.skill) == -1) {
                    coopnet_report_glitch("action_use_skill_on failed (skill=%d)\n", request.skill);
                    g_coopCompanionActionBusy = false;
                }
            }
            return;
        }
    }

    coopnet_report_glitch("queued companion action had no matching target (kind=%d, pid=%d, tile=%d)\n", request.kind, evt.pid, evt.tile);
}

// ---------------------------------------------------------------------------
// Character progress: the client's character is a real character file that
// levels up on the client's PC; the host keeps a copy with each of ITS saves
// (the "record") and hands it back on reconnect, so a character that gained
// levels, perks and skill points in one host's world comes back as it was.
// ---------------------------------------------------------------------------

static void coopnet_chars_ensure_dir();
static void coopnet_client_send_character();

// Client: the co-op character file the player picked on the main menu (empty
// when they joined some other way). Saved again whenever the character changes.
static char g_coopActiveCharPath[80] = "";

// Host: this world's record of the client's character (blobLen 0 = none yet).
// Written into the host's save slot with the rest of the client's profile.
static CoopCharBlob g_coopHostCharRecord;

// Host: the character as the client joined this session (for a save that has no
// record of its own), the experience of the last progress report the host
// accepted, and the experience the host itself handed out since then.
static CoopCharBlob g_coopHostJoinBaseline;
static int32_t g_coopHostAcceptedXp = -1;
static int64_t g_coopHostXpGranted = 0;

static const char* const kCoopCharTransferPath = "COOPCHARS\\_xfer.tmp";

static bool coopnet_client_build_blob(CoopCharBlob& out, uint8_t kind)
{
    memset(&out, 0, sizeof(out));
    out.kind = kind;
    snprintf(out.name, sizeof(out.name), "%s", critter_name(obj_dude));
    out.level = stat_pc_get(PC_STAT_LEVEL);
    out.xp = stat_pc_get(PC_STAT_EXPERIENCE);

    coopnet_chars_ensure_dir();
    if (pc_coop_save_data(kCoopCharTransferPath) != 0) {
        coopnet_report_glitch("Couldn't package the character for the host (save failed)");
        return false;
    }

    DB_FILE* stream = db_fopen(kCoopCharTransferPath, "rb");
    if (stream == NULL) {
        return false;
    }
    long length = db_filelength(stream);
    if (length <= 0 || length > kCoopCharBlobMax) {
        db_fclose(stream);
        coopnet_report_glitch("Character file is %ld bytes, over the %d the host can take", length, kCoopCharBlobMax);
        return false;
    }
    size_t got = db_fread(out.blob, 1, static_cast<size_t>(length), stream);
    db_fclose(stream);
    if (got != static_cast<size_t>(length)) {
        return false;
    }
    out.blobLen = static_cast<int32_t>(length);
    return true;
}

static void coopnet_client_send_blob(uint8_t kind)
{
    CoopCharBlob blob;
    if (!coopnet_client_build_blob(blob, kind)) {
        return;
    }
    coopnet_send_message(g_coopPeerSocket, COOP_MSG_CHAR_BLOB, &blob, sizeof(blob));
    debug_printf("\nCoop: sent CHAR_BLOB kind=%d name=%s level=%d xp=%d bytes=%d\n", kind, blob.name, blob.level, blob.xp, blob.blobLen);
}

// Client: keep the picked character file current.
static void coopnet_client_save_active_character()
{
    if (g_coopActiveCharPath[0] == '\0') {
        return;
    }
    coopnet_chars_ensure_dir();
    if (pc_coop_save_data(g_coopActiveCharPath) != 0) {
        coopnet_report_glitch("Couldn't update the character file %s", g_coopActiveCharPath);
    }
}

// Client: the companion this client plays carries a copy of the player's
// prototype data; after a level-up or a character screen visit that copy is
// stale (max HP, skills...), so refresh it.
static void coopnet_client_refresh_companion_from_pc()
{
    Proto* source = NULL;
    Proto* dest = coopnet_ensure_companion_proto();
    if (dest == NULL || proto_ptr(0x1000000, &source) == -1 || source == NULL) {
        return;
    }
    memcpy(&(dest->critter.data), &(source->critter.data), sizeof(CritterProtoData));

    if (g_coopCompanion != NULL && g_coopCompanion->pid == kCoopCompanionPid) {
        int maxHp = stat_level(g_coopCompanion, STAT_MAXIMUM_HIT_POINTS);
        if (g_coopCompanion->data.critter.hp > maxHp) {
            g_coopCompanion->data.critter.hp = maxHp;
        }
    }
}

// Client: something about the character changed (XP, a level, spent skill
// points, a chosen perk): tell the host and keep the local file current.
static void coopnet_client_character_changed()
{
    coopnet_client_refresh_companion_from_pc();
    coopnet_client_save_active_character();
    if (g_coopRole == CoopRole::Client && g_coopConnState == CoopConnState::Connected) {
        coopnet_client_send_character();
        coopnet_client_send_blob(1);
    }
}

// Client: the host has a record of this character with MORE progress than the
// copy we joined with -- take it.
static void coopnet_client_apply_restore(const CoopCharBlob& restore)
{
    if (restore.blobLen <= 0 || restore.blobLen > kCoopCharBlobMax) {
        return;
    }

    coopnet_chars_ensure_dir();
    DB_FILE* stream = db_fopen(kCoopCharTransferPath, "wb");
    if (stream == NULL) {
        return;
    }
    size_t wrote = db_fwrite(restore.blob, 1, static_cast<size_t>(restore.blobLen), stream);
    db_fclose(stream);
    if (wrote != static_cast<size_t>(restore.blobLen)) {
        return;
    }

    if (pc_coop_load_data(kCoopCharTransferPath) != 0) {
        coopnet_report_glitch("The host's saved copy of the character couldn't be read");
        return;
    }
    stat_recalc_derived(obj_dude);
    proto_dude_update_gender();

    coopnet_status(COOP_STATUS_GOOD, "Your character was restored from the host's records: %s, level %d.", restore.name, restore.level);
    coopnet_client_character_changed();
}

// Host: the client sent its character.
static void coopnet_host_on_char_blob(const CoopCharBlob& incoming)
{
    if (incoming.blobLen <= 0 || incoming.blobLen > kCoopCharBlobMax) {
        return;
    }

    bool sameCharacter = g_coopHostCharRecord.blobLen > 0 && strncmp(g_coopHostCharRecord.name, incoming.name, sizeof(incoming.name) - 1) == 0;

    // The host's save is the authority on how far the client's character got in
    // THIS world -- in both directions. On joining, a character this world
    // already knows is set back to the record, whether the client arrived with
    // more experience (progress made in some other session or by loading an
    // older save of its own) or less.
    if (incoming.kind == 0 && sameCharacter
        && (g_coopHostCharRecord.xp != incoming.xp || g_coopHostCharRecord.level != incoming.level)) {
        CoopCharBlob restore = g_coopHostCharRecord;
        restore.kind = 0;
        coopnet_send_message(g_coopPeerSocket, COOP_MSG_CHAR_RESTORE, &restore, sizeof(restore));
        coopnet_status(COOP_STATUS_INFO, "Set %s back to this world's record (level %d, %d XP).", restore.name, restore.level, restore.xp);
        g_coopHostJoinBaseline = g_coopHostCharRecord;
        g_coopHostAcceptedXp = restore.xp;
        g_coopHostXpGranted = 0;
        return;
    }

    if (incoming.kind == 0) {
        g_coopHostJoinBaseline = incoming;
        g_coopHostJoinBaseline.kind = 0;
    } else if (g_coopHostAcceptedXp >= 0) {
        // A progress update: the experience can only have risen by what this
        // host handed out (the client's perks may add a few percent). More than
        // that means the client brought experience from somewhere else -- undo it.
        int64_t allowed = static_cast<int64_t>(g_coopHostAcceptedXp) + g_coopHostXpGranted * 5 / 4 + 100;
        if (incoming.xp > allowed) {
            const CoopCharBlob* source = g_coopHostCharRecord.blobLen > 0 ? &g_coopHostCharRecord : &g_coopHostJoinBaseline;
            if (source->blobLen > 0) {
                CoopCharBlob restore = *source;
                restore.kind = 0;
                coopnet_send_message(g_coopPeerSocket, COOP_MSG_CHAR_RESTORE, &restore, sizeof(restore));
                coopnet_status(COOP_STATUS_WARN, "%s reported more experience than this world gave (%d, expected at most %d) - set back to level %d.", incoming.name, incoming.xp, static_cast<int>(allowed), restore.level);
                g_coopHostAcceptedXp = restore.xp;
                g_coopHostXpGranted = 0;
                return;
            }
        }
    }

    g_coopHostCharRecord = incoming;
    g_coopHostCharRecord.kind = 0;
    g_coopHostAcceptedXp = incoming.xp;
    g_coopHostXpGranted = 0;
    debug_printf("\nCoop: stored character record name=%s level=%d xp=%d (from kind=%d)\n", incoming.name, incoming.level, incoming.xp, incoming.kind);
}

// Host: a save was just loaded while the client is connected. The save decides
// how far the client's character is: its own record if it has one, else the
// character as it joined this session (a save that predates the client). Either
// way the client is set back -- reloading to kill the same enemies again does
// not keep the experience.
static void coopnet_host_push_character_record()
{
    if (g_coopRole != CoopRole::Host || g_coopConnState != CoopConnState::Connected) {
        return;
    }

    if (g_coopHostCharRecord.blobLen <= 0 && g_coopHostJoinBaseline.blobLen > 0) {
        g_coopHostCharRecord = g_coopHostJoinBaseline;
    }
    if (g_coopHostCharRecord.blobLen <= 0) {
        return;
    }

    CoopCharBlob restore = g_coopHostCharRecord;
    restore.kind = 0;
    coopnet_send_message(g_coopPeerSocket, COOP_MSG_CHAR_RESTORE, &restore, sizeof(restore));
    g_coopHostAcceptedXp = restore.xp;
    g_coopHostXpGranted = 0;
    coopnet_status(COOP_STATUS_INFO, "Loaded save: %s is back at level %d (%d XP), as in this save.", restore.name, restore.level, restore.xp);
}

// Client: sends this machine's own character to the host so the companion it
// plays has the client's real stats and skills instead of the host's.
static void coopnet_client_send_character()
{
    CoopCharacter msg;
    memset(&msg, 0, sizeof(msg));
    snprintf(msg.name, sizeof(msg.name), "%s", critter_name(obj_dude));
    for (int i = 0; i < 7; i++) {
        msg.special[i] = stat_get_base(obj_dude, i);
    }
    for (int i = 0; i < SKILL_COUNT && i < 18; i++) {
        msg.skills[i] = skill_level(obj_dude, i);
    }
    msg.level = stat_pc_get(PC_STAT_LEVEL);
    msg.hpBonus = stat_get_bonus(obj_dude, STAT_MAXIMUM_HIT_POINTS);

    // The client's own screen shows its character under its own name.
    snprintf(g_coopCompanionName, sizeof(g_coopCompanionName), "%s", msg.name);

    bool sent = coopnet_send_message(g_coopPeerSocket, COOP_MSG_CHARACTER, &msg, sizeof(msg));
    coopnet_status(COOP_STATUS_INFO, "Sent your character (%s, level %d) to the host.", msg.name, msg.level);
    debug_printf("\nCoop: sent CHARACTER name=%s S%d P%d E%d C%d I%d A%d L%d success=%d\n", msg.name,
        msg.special[0], msg.special[1], msg.special[2], msg.special[3], msg.special[4], msg.special[5], msg.special[6], sent);
}

// Host: the client's character arrived -- give the companion its numbers.
static void coopnet_host_apply_client_character(const CoopCharacter& c)
{
    if (g_coopCompanion == NULL) {
        return;
    }

    Proto* proto = coopnet_ensure_companion_proto();
    if (proto == NULL || g_coopCompanion->pid != kCoopCompanionPid) {
        coopnet_report_glitch("Client's character couldn't be applied: the companion has no data of its own (shares the host's)");
        return;
    }

    int oldMax = stat_level(g_coopCompanion, STAT_MAXIMUM_HIT_POINTS);

    CritterProtoData* data = &(proto->critter.data);
    int oldHpBonus = data->bonusStats[STAT_MAXIMUM_HIT_POINTS];
    for (int i = 0; i < 7; i++) {
        int value = c.special[i];
        if (value < 1) {
            value = 1;
        } else if (value > 10) {
            value = 10;
        }
        data->baseStats[i] = value;
        data->bonusStats[i] = 0;
    }
    data->bonusStats[STAT_MAXIMUM_HIT_POINTS] = c.hpBonus;
    stat_recalc_derived(g_coopCompanion);

    for (int skill = 0; skill < SKILL_COUNT && skill < 18; skill++) {
        data->skills[skill] = skill_points_for_level(g_coopCompanion, skill, c.skills[skill]);
    }

    int newMax = stat_level(g_coopCompanion, STAT_MAXIMUM_HIT_POINTS);
    int hp = g_coopCompanion->data.critter.hp;
    if (hp > 0 && c.hpBonus > oldHpBonus && oldHpBonus >= 0) {
        // A level-up heals by what it adds, like it does for a normal player.
        hp += c.hpBonus - oldHpBonus;
        g_coopCompanion->data.critter.hp = hp > newMax ? newMax : hp;
    } else if (hp > 0 && (hp >= oldMax || hp > newMax)) {
        g_coopCompanion->data.critter.hp = newMax;
    }

    char name[32];
    snprintf(name, sizeof(name), "%s", c.name);
    name[sizeof(name) - 1] = '\0';
    if (name[0] != '\0') {
        snprintf(g_coopCompanionName, sizeof(g_coopCompanionName), "%s", name);
    }

    coopnet_status(COOP_STATUS_GOOD, "Your friend's character: %s (ST %d PE %d EN %d CH %d IN %d AG %d LK %d), %d HP.",
        g_coopCompanionName, c.special[0], c.special[1], c.special[2], c.special[3], c.special[4], c.special[5], c.special[6], newMax);
}

// Host: tells the client which game/combat difficulty is actually in force.
// Sent at connect and every few seconds, so a client that changes its own in
// the options screen gets pulled straight back to the host's.
static void coopnet_host_broadcast_settings()
{
    static uint32_t lastSentMs = 0;
    uint32_t now = coopnet_now_ms();
    if (g_coopConnState != CoopConnState::Connected || now - lastSentMs < 5000) {
        return;
    }
    lastSentMs = now;

    CoopSettings settings;
    settings.gameDifficulty = 1;
    settings.combatDifficulty = 1;
    settings.combatSpeed = 0;
    settings.playerSpeedup = 0;
    config_get_value(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_PLAYER_SPEEDUP_KEY, &settings.playerSpeedup);
    config_get_value(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_GAME_DIFFICULTY_KEY, &settings.gameDifficulty);
    config_get_value(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_COMBAT_DIFFICULTY_KEY, &settings.combatDifficulty);
    config_get_value(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_COMBAT_SPEED_KEY, &settings.combatSpeed);
    coopnet_send_message(g_coopPeerSocket, COOP_MSG_SETTINGS, &settings, sizeof(settings));
}

static void coopnet_poll_host()
{
    coopnet_host_autosave_profile();
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
            g_coopHostIncomingAttempts++;
            char fromText[48] = "?";
            inet_ntop(AF_INET, &clientAddr.sin_addr, fromText, sizeof(fromText));
            coopnet_status(COOP_STATUS_INFO, "Connection #%d arrived from %s - waiting for its hello...", g_coopHostIncomingAttempts, fromText);
            coopnet_set_nonblocking(accepted);
            g_coopPeerSocket = accepted;
            g_coopRecvBufferLen = 0;
            g_coopConnState = CoopConnState::WaitingForHello;
            g_coopPeerClosed = false;
            g_coopLastRecvTimeMs = coopnet_now_ms();
            g_coopHostHelloStartMs = g_coopLastRecvTimeMs;
        } else if (g_coopHostIncomingAttempts == 0 && !g_coopHostNoAttemptWarned
            && coopnet_now_ms() - g_coopHostListenStartMs > 60000) {
            // Nothing has even knocked. The problem is between the two PCs,
            // not in either game -- say where to look.
            g_coopHostNoAttemptWarned = true;
            coopnet_status(COOP_STATUS_WARN, "1 minute and no connection attempt has reached this PC at all.");
            coopnet_status(COOP_STATUS_WARN, "Check: your friend typed one of the addresses shown above, you're both");
            coopnet_status(COOP_STATUS_WARN, "on the same Radmin/VPN network, and Windows Firewall isn't blocking this");
            coopnet_status(COOP_STATUS_WARN, "game (run Allow_Firewall.bat). His screen shows the exact error.");
        }
    } else if (g_coopConnState == CoopConnState::Connected) {
        // A friend whose game crashed/restarted reconnects before this side
        // has noticed the old connection died (it can take up to the 2
        // minute heartbeat timeout). The newcomer is by far the likelier
        // real one -- take the new connection instead of ignoring it for
        // minutes, which looked exactly like "he can't join".
        sockaddr_in clientAddr;
#ifdef _WIN32
        int addrLen = sizeof(clientAddr);
#else
        socklen_t addrLen = sizeof(clientAddr);
#endif
        CoopSocket fresh = accept(g_coopListenSocket, reinterpret_cast<sockaddr*>(&clientAddr), &addrLen);
        if (fresh != COOP_INVALID_SOCKET) {
            g_coopHostIncomingAttempts++;
            char fromText[48] = "?";
            inet_ntop(AF_INET, &clientAddr.sin_addr, fromText, sizeof(fromText));
            coopnet_status(COOP_STATUS_WARN, "Connection #%d arrived from %s while a friend was still marked connected - replacing the old connection.", g_coopHostIncomingAttempts, fromText);
            coopnet_close_socket(g_coopPeerSocket);
            coopnet_host_on_client_lost();
            coopnet_set_nonblocking(fresh);
            g_coopPeerSocket = fresh;
            g_coopRecvBufferLen = 0;
            g_coopConnState = CoopConnState::WaitingForHello;
            g_coopPeerClosed = false;
            g_coopLastRecvTimeMs = coopnet_now_ms();
            g_coopHostHelloStartMs = g_coopLastRecvTimeMs;
        }
    }

    if (g_coopConnState == CoopConnState::WaitingForHello) {
        uint8_t type;
        unsigned char payload[kCoopMaxMessagePayload];
        uint16_t payloadLen;
        bool gotMessage = coopnet_try_recv_message(g_coopPeerSocket, &type, payload, &payloadLen);
        if (!gotMessage && (g_coopPeerClosed || coopnet_now_ms() - g_coopHostHelloStartMs > kCoopHostHelloTimeoutMs)) {
            coopnet_status(COOP_STATUS_WARN, "A connection arrived but never completed the handshake (%s) - listening again.",
                g_coopPeerClosed ? "it closed" : "10s with no hello");
            coopnet_close_socket(g_coopPeerSocket);
            g_coopRecvBufferLen = 0;
            g_coopConnState = CoopConnState::Listening;
        } else if (gotMessage) {
            debug_printf("\nCoop: host received message type=%d len=%d while waiting for HELLO\n", type, payloadLen);
            if (type == COOP_MSG_HELLO && payloadLen == sizeof(CoopHello)) {
                CoopHello hello;
                memcpy(&hello, payload, sizeof(hello));
                debug_printf("\nCoop: HELLO received (protocolVersion=%u, mapName=%.16s)\n", hello.protocolVersion, hello.mapName);

                if (hello.protocolVersion != kCoopProtocolVersion) {
                    CoopHelloAck refusal;
                    memset(&refusal, 0, sizeof(refusal));
                    refusal.accepted = 0;
                    refusal.companionPid = -1;
                    coopnet_send_message(g_coopPeerSocket, COOP_MSG_HELLO_ACK, &refusal, sizeof(refusal));
                    coopnet_status(COOP_STATUS_BAD, "Refused a friend: his game is protocol %u, yours is %u.", hello.protocolVersion, kCoopProtocolVersion);
                    coopnet_status(COOP_STATUS_BAD, "You both need the exact same fallout-ce.exe.");
                    coopnet_close_socket(g_coopPeerSocket);
                    g_coopRecvBufferLen = 0;
                    g_coopConnState = CoopConnState::Listening;
                    return;
                }

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

                // Bring the client to the host's current map/position right
                // away. It used to be assumed both sides had loaded the same
                // save before connecting -- fine for two windows on one PC,
                // a real setup burden across two machines (the client could
                // otherwise sit on a completely different map, seeing the
                // host's positions applied to the wrong world).
                if (map_data.name[0] != '\0') {
                    CoopMapTransition sync;
                    memset(&sync, 0, sizeof(sync));
                    strncpy(sync.mapName, map_data.name, sizeof(sync.mapName) - 1);
                    sync.tile = obj_dude->tile;
                    sync.elevation = obj_dude->elevation;
                    sync.rotation = obj_dude->rotation;
                    sync.sameMapElevationOnly = 0;
                    g_coopGvarShadowValid = false;
                    coopnet_host_reset_world_shadow();
                    bool syncSent = coopnet_send_message(g_coopPeerSocket, COOP_MSG_MAP_TRANSITION, &sync, sizeof(sync));
                    debug_printf("\nCoop: sent connect-time MAP_TRANSITION to %.16s success=%d\n", sync.mapName, syncSent);
                }

                coopnet_status(COOP_STATUS_GOOD, "Your friend connected!");
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
            } else if (type == COOP_MSG_CHARACTER && payloadLen == sizeof(CoopCharacter)) {
                CoopCharacter character;
                memcpy(&character, payload, sizeof(character));
                coopnet_host_apply_client_character(character);
            } else if (type == COOP_MSG_CHAR_BLOB && payloadLen == sizeof(CoopCharBlob)) {
                CoopCharBlob blob;
                memcpy(&blob, payload, sizeof(blob));
                coopnet_host_on_char_blob(blob);
            } else if (type == COOP_MSG_RESYNC_REQUEST) {
                // The client loaded a save: its objects (companion included) are
                // gone. Forget what it has and send everything again.
                g_coopHostLastMapName[0] = '\0';
                g_coopHostResyncOnly = true;
                coopnet_host_reset_world_shadow();
                coopnet_host_broadcast_companion_inventory();
                debug_printf("\nCoop: client asked for a full resync\n");

                // A client that reloaded in the middle of a fight lost its combat
                // state (the end-turn buttons, whose turn it is): tell it again.
                if (isInCombat() && g_coopCompanion != NULL) {
                    coopnet_send_message(g_coopPeerSocket, COOP_MSG_COMBAT_BEGIN, NULL, 0);
                    CoopCombatTurn turn;
                    turn.actionPoints = g_coopHostCombatTurnActive ? g_coopCompanion->data.critter.combat.ap : 0;
                    coopnet_send_message(g_coopPeerSocket, COOP_MSG_COMBAT_TURN, &turn, sizeof(turn));
                    debug_printf("\nCoop: re-sent combat state to the client (turnActive=%d ap=%d)\n", g_coopHostCombatTurnActive ? 1 : 0, turn.actionPoints);
                }
            } else if (type == COOP_MSG_USE_ITEM && payloadLen == sizeof(CoopUseItem)) {
                CoopUseItem req;
                memcpy(&req, payload, sizeof(req));
                Object* found = NULL;
                if (g_coopCompanion != NULL) {
                    Inventory* inv = &(g_coopCompanion->data.inventory);
                    for (int i = 0; i < inv->length; i++) {
                        Object* candidate = inv->items[i].item;
                        if (candidate->pid == req.pid && (candidate->flags & OBJECT_EQUIPPED) == 0) {
                            found = candidate;
                            break;
                        }
                    }
                    if (found == NULL) {
                        // Only equipped copies (e.g. a wielded flare/knife).
                        for (int i = 0; i < inv->length; i++) {
                            if (inv->items[i].item->pid == req.pid) {
                                found = inv->items[i].item;
                                break;
                            }
                        }
                    }
                }
                debug_printf("\nCoop: received USE_ITEM pid=%d onHostDude=%d found=%p\n", req.pid, req.onHostDude, (void*)found);
                if (found != NULL) {
                    if (req.onHostDude) {
                        obj_use_item_on(g_coopCompanion, obj_dude, found);
                    } else {
                        obj_use_item(g_coopCompanion, found);
                    }
                    coopnet_refresh_critter_fid(g_coopCompanion);
                    coopnet_host_broadcast_companion_inventory();
                }
            } else if (type == COOP_MSG_LOOT_REQUEST && payloadLen == sizeof(CoopItemEvent)) {
                CoopItemEvent evt;
                memcpy(&evt, payload, sizeof(evt));
                debug_printf("\nCoop: received LOOT_REQUEST from client (pid=%d, tile=%d)\n", evt.pid, evt.tile);
                coopnet_enqueue_companion_action(COOP_COMPANION_ACTION_LOOT, evt);
            } else if (type == COOP_MSG_SKILL_REQUEST && payloadLen == sizeof(CoopSkillRequest)) {
                CoopSkillRequest req;
                memcpy(&req, payload, sizeof(req));
                debug_printf("\nCoop: received SKILL_REQUEST from client (skill=%d, pid=%d, tile=%d, targetIsHostDude=%d)\n", req.skill, req.pid, req.tile, req.targetIsHostDude);
                CoopItemEvent evt;
                evt.pid = req.pid;
                evt.tile = req.tile;
                evt.elevation = req.elevation;
                coopnet_enqueue_companion_action(COOP_COMPANION_ACTION_SKILL, evt, req.skill, req.targetIsHostDude != 0);
            } else if (type == COOP_MSG_DIALOGUE_START_REQUEST && payloadLen == sizeof(CoopItemEvent)) {
                CoopItemEvent talkEvt;
                memcpy(&talkEvt, payload, sizeof(talkEvt));
                coopnet_host_apply_dialogue_start(talkEvt);
            } else if (type == COOP_MSG_REMOTE_INPUT && payloadLen == sizeof(CoopRemoteInput)) {
                CoopRemoteInput remoteIn;
                memcpy(&remoteIn, payload, sizeof(remoteIn));
                coopnet_remote_host_apply_input(remoteIn);
            } else if (type == COOP_MSG_DIALOGUE_PICK && payloadLen == sizeof(int32_t)) {
                if (g_coopDialogueDrivenByClient) {
                    int32_t pick;
                    memcpy(&pick, payload, sizeof(pick));
                    g_coopPendingDialoguePick = pick;
                }
            } else if (type == COOP_MSG_COMBAT_START_REQUEST && payloadLen == sizeof(CoopItemEvent)) {
                CoopItemEvent startEvt;
                memcpy(&startEvt, payload, sizeof(startEvt));
                coopnet_host_apply_combat_start(startEvt);
            } else if (type == COOP_MSG_TIME_ADVANCE && payloadLen == sizeof(CoopGameTime)) {
                // The client rested. Game time is host-authoritative (and
                // pushed back to the client every change), so without this
                // its rest was silently reverted and the host never saw
                // night fall. Only ever moves the clock forward.
                CoopGameTime requested;
                memcpy(&requested, payload, sizeof(requested));
                if (requested.gameTime > game_time()) {
                    set_game_time(requested.gameTime);
                    if (map_script_id != -1) {
                        scr_exec_map_update_scripts();
                    }
                }
            } else if (type == COOP_MSG_INVENTORY_AP_REQUEST) {
                // Same formula handle_inventory() (inventry.cc) already uses
                // for obj_dude's own identical case -- recomputed here
                // rather than trusting a client-supplied cost, so the two
                // can never drift out of formula-sync with each other.
                // Only applied during the companion's own turn (matching
                // vanilla's "not your turn" restriction) and clamped at 0
                // (never lets AP go negative). The resulting change gets
                // picked up and re-broadcast to the client automatically by
                // coopnet_combat_input()'s own AP-diff loop -- no separate
                // reply message needed here.
                if (g_coopHostCombatTurnActive && g_coopCompanion != NULL) {
                    int apCost = 4 - perk_level(PERK_QUICK_POCKETS);
                    if (apCost > 0) {
                        int newAp = g_coopCompanion->data.critter.combat.ap - apCost;
                        if (newAp < 0) {
                            newAp = 0;
                        }
                        g_coopCompanion->data.critter.combat.ap = newAp;
                        debug_printf("\nCoop: received INVENTORY_AP_REQUEST, deducted %d AP (now %d)\n", apCost, newAp);
                    }
                } else {
                    debug_printf("\nCoop: ignored INVENTORY_AP_REQUEST, not the companion's turn\n");
                }
            } else if (type == COOP_MSG_INVENTORY_PUSH && payloadLen == sizeof(CoopInventorySync)) {
                CoopInventorySync push;
                memcpy(&push, payload, sizeof(push));
                g_coopCompanionActiveHand = push.activeHand != 0 ? 1 : 0;
                if (g_coopCompanion != NULL && g_coopCompanion->data.inventory.length <= kCoopMaxInventorySyncItems) {
                    coopnet_apply_companion_inventory(push);
                    coopnet_refresh_critter_fid(g_coopCompanion);
                    debug_printf("\nCoop: applied INVENTORY_PUSH from client (%d items)\n", push.itemCount);
                } else {
                    debug_printf("\nCoop: ignored INVENTORY_PUSH (companion inventory too large to replace safely)\n");
                }
            } else if (type == COOP_MSG_COMBAT_ACTION && payloadLen == sizeof(CoopCombatAction)) {
                if (!g_coopHostCombatTurnActive) {
                    // Stray/late message outside the companion's actual
                    // turn window -- see g_coopHostCombatTurnActive's comment.
                    debug_printf("\nCoop: ignored COMBAT_ACTION, not the companion's turn\n");
                } else {
                    CoopCombatAction action;
                    memcpy(&action, payload, sizeof(action));
                    if (action.actionType == COOP_COMBAT_ACTION_END_TURN) {
                        debug_printf("\nCoop: received COMBAT_ACTION end-turn\n");
                        g_coopHostCombatEndTurnRequested = true;
                    } else if (g_coopCompanion != NULL && anim_busy(g_coopCompanion)) {
                        // A shot or a walk is still playing. Starting another
                        // action now clears the whole running animation sequence,
                        // which also holds the target's fall -- a killed enemy
                        // stayed standing on the host. Do it once the animation
                        // is over (the newest request replaces an older one).
                        g_coopHostPendingCombatAction = action;
                        g_coopHostPendingCombatActionValid = true;
                        debug_printf("\nCoop: COMBAT_ACTION type=%d held until the companion is idle\n", action.actionType);
                    } else {
                        coopnet_host_run_combat_action(action);
                    }
                }
            }
        }

        uint32_t now = coopnet_now_ms();
        if (now - g_coopLastRecvTimeMs > kCoopHeartbeatTimeoutMs || g_coopPeerClosed) {
            disconnected = true;
        }

        if (!disconnected) {
            coopnet_host_process_action_queue();
        }

        if (!disconnected && now - g_coopLastBroadcastTimeMs >= kCoopBroadcastIntervalMs) {
            // Repeating state: a tick the peer has no room for is just skipped.
            bool wasDroppable = g_coopSendDroppable;
            g_coopSendDroppable = true;
            coopnet_host_broadcast_positions();
            coopnet_host_broadcast_game_time();
            coopnet_host_broadcast_combat_participants();
            coopnet_host_broadcast_world();
            coopnet_host_broadcast_settings();
            g_coopSendDroppable = wasDroppable;
            // Changes are only sent once, so these may wait for the peer.
            coopnet_host_broadcast_gvars();
            g_coopLastBroadcastTimeMs = now;
        }

        if (disconnected) {
            coopnet_status(COOP_STATUS_WARN, "Your friend disconnected (%s). Still hosting - he can rejoin.",
                g_coopPeerClosed ? "his game closed or crashed" : "no data for 2 minutes");
            coopnet_close_socket(g_coopPeerSocket);
            g_coopRecvBufferLen = 0;
            g_coopConnState = CoopConnState::Listening;
            coopnet_host_on_client_lost();
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

    // Applied unconditionally, ahead of the position-based branching below
    // -- HP can change while genuinely standing still (getting shot in
    // place), so it can't be gated behind any of those early returns.
    if (target->data.critter.hp != pos.hp) {
        int oldHp = target->data.critter.hp;
        target->data.critter.hp = pos.hp;

        // This function is now coop-aware (intface.cc): on the client it
        // shows the companion's HP instead of obj_dude's, since the
        // companion is the client's actual played character -- see its
        // own comment. This function is client-side only (the name says
        // so), so that redirect always applies here; calling it
        // unconditionally on any HP change (whichever object it was for)
        // keeps the one on-screen number correct either way.
        intface_update_hit_points(true);

        // The generic "Companion takes damage!" placeholder that used to
        // live here is gone -- combat_display()'s own real text
        // (miss/hit/damage amount/critical/death) now reaches the client
        // directly via COOP_MSG_COMBAT_TEXT (see
        // coopnet_begin_capture_combat_text()'s comment), so this would
        // just be redundant noise alongside it for the normal case. Any
        // non-combat source of HP change (rare) now has no message at
        // all, same acceptable tradeoff as other untracked edge cases.
        (void)oldHp;
    }

    // Transparency (stealth boy etc.): the host's flags decide.
    if ((target->flags & OBJECT_FLAG_0xFC000) != (pos.transFlags & OBJECT_FLAG_0xFC000)) {
        target->flags = (target->flags & ~OBJECT_FLAG_0xFC000) | (pos.transFlags & OBJECT_FLAG_0xFC000);
        Rect transRect;
        obj_bound(target, &transRect);
        tile_refresh_rect(&transRect, target->elevation);
    }

    // Equipped weapon/armor look: swap in the host's base art + weapon code
    // while keeping this object's own animation type and facing. Skipped for
    // a dead body, and if the resulting art doesn't exist (e.g. a running
    // animation for a weapon that has none) the change is just not applied.
    if (FID_TYPE(target->fid) == OBJ_TYPE_CRITTER && (target->data.critter.combat.results & DAM_DEAD) == 0
        && ((target->fid & 0xFFF) != pos.fidBase || ((target->fid & 0xF000) >> 12) != pos.weaponCode)) {
        int newFid = art_id(OBJ_TYPE_CRITTER, pos.fidBase, FID_ANIM_TYPE(target->fid), pos.weaponCode, (target->fid & 0x70000000) >> 28);
        if (art_exists(newFid)) {
            Rect fidRect;
            obj_change_fid(target, newFid, &fidRect);
            tile_refresh_rect(&fidRect, target->elevation);
        }
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

    // isInCombat() here is the CLIENT's own local combat state, not the
    // host's real synced fight -- NPCs run independently on each side, so
    // the client's own nearby (mirrored) hostiles can perfectly well tip
    // its own local simulation into combat mode on their own, regardless
    // of what the host's real fight is doing. That matters here because
    // register_end() (below, in the smooth-run branch) checks this exact
    // same global flag: when true, it routes the animation through
    // combat_anim_begin()'s bookkeeping instead of just playing it, which
    // expects a real combat_turn() cycle to eventually release it -- the
    // client never runs one. Confirmed via user testing as a real bug:
    // the companion's sprite visually froze in place on the client's own
    // screen while genuinely moving (correctly) on the host's, because the
    // queued animation just never got a chance to actually play. Forcing
    // the instant-snap path instead of the smooth run whenever the
    // client's local isInCombat() is true sidesteps register_end()'s
    // combat branch entirely -- less smooth-looking during a fight, but
    // guaranteed to actually show up, which matters a lot more.
    // A full-path walk/run from COOP_MSG_MOVE_ANIM is underway: let it
    // finish rather than restarting a short run toward every snapshot.
    // Falls through to the normal snap/chase logic once it arrives, times
    // out, or the object has drifted too far from the host to trust it.
    if (g_coopMoveDest[pos.which] != -1) {
        bool arrived = target->tile == g_coopMoveDest[pos.which]
            || (FID_ANIM_TYPE(target->fid) == ANIM_STAND && coopnet_now_ms() - g_coopMoveDestStartMs[pos.which] > 300);
        bool timedOut = coopnet_now_ms() - g_coopMoveDestStartMs[pos.which] > kCoopMoveDestTimeoutMs;
        bool drifted = target->elevation != pos.elevation || tile_dist(target->tile, pos.tile) > 6;
        if (arrived || timedOut || drifted || isInCombat()) {
            debug_printf("\nCoop: client move dest cleared which=%d arrived=%d timedOut=%d drifted=%d objTile=%d hostTile=%d dest=%d\n",
                pos.which, arrived, timedOut, drifted, target->tile, pos.tile, g_coopMoveDest[pos.which]);
            g_coopMoveDest[pos.which] = -1;

            // The walk did NOT finish (drifted / timed out): this client's own
            // path-finding can fail or be blocked where the host's wasn't -- e.g.
            // a door that's open on the host but still closed in the client's
            // own copy of the world -- so the run never started or stalled.
            // Leaving g_coopLastCommandedTile pointing at the unreachable
            // destination made the chase below wait forever for an arrival that
            // couldn't happen, so the character sat still until the host was
            // far away (confirmed via testing: "can't walk around freely").
            // Forget the stale destination and simply jump to where the host
            // says the character is.
            if (!arrived) {
                g_coopLastCommandedTile[pos.which] = -1;
                if (timedOut || drifted) {
                    register_clear(target);
                    Rect snapRect;
                    obj_move_to_tile(target, pos.tile, pos.elevation, &snapRect);
                    obj_set_rotation(target, pos.rotation, &snapRect);
                    tile_refresh_rect(&snapRect, pos.elevation);
                    g_coopLastCommandedTile[pos.which] = pos.tile;
                    return;
                }
            }
        } else {
            return;
        }
    }

    if (target->elevation != pos.elevation || tile_dist(target->tile, pos.tile) > kCoopSnapDistanceThreshold || isInCombat()) {
        // A jump this big is a teleport (an encounter map placing the party
        // somewhere other than the transition tile, a new location): the
        // camera was centered on the OLD spot, so the client's view would
        // show empty ground far from its character -- confirmed via testing
        // ("spawns in a random place" after encounters / entering the Hub).
        bool bigJump = pos.which == 0 && (target->elevation != pos.elevation || tile_dist(target->tile, pos.tile) > 20);
        Rect rect;
        obj_move_to_tile(target, pos.tile, pos.elevation, &rect);
        obj_set_rotation(target, pos.rotation, &rect);
        tile_refresh_rect(&rect, pos.elevation);
        g_coopLastCommandedTile[pos.which] = pos.tile;
        if (bigJump && !g_coopClientMapLoading) {
            if (elevationIsValid(pos.elevation) && pos.elevation != map_elevation) {
                map_set_elevation(pos.elevation);
            }
            tile_set_center(pos.tile, TILE_SET_CENTER_REFRESH_WINDOW);
        }
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
    g_coopSanctionedMoveDepth++;
    register_begin(ANIMATION_REQUEST_UNRESERVED);
    register_object_run_to_tile(target, pos.tile, pos.elevation, -1, 0);
    register_end();
    g_coopSanctionedMoveDepth--;
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

// Client-side: the reverse of coopnet_resolve_anim_id() (host side) --
// turns a CoopCombatAttackAnim/CoopCombatDamageAnim id back into a real
// local object. -1 (not companion/obj_dude/a tracked participant) comes
// back NULL; callers skip animating rather than guessing.
static Object* coopnet_resolve_anim_object(int32_t id)
{
    if (id == kCoopAnimIdCompanion) {
        return g_coopCompanion;
    }
    if (id == kCoopCombatTargetHostDude) {
        return obj_dude;
    }
    if (id == -1) {
        return NULL;
    }
    return coopnet_find_participant_object(id);
}

// Client-side only: nonzero while one of OUR OWN network-driven move
// registrations (COOP_MSG_MOVE_ANIM, position chase, participant mirroring)
// is being made. Anything else asking to move the companion locally on the
// client (an NPC script/AI running in the client's own independent
// simulation, e.g. during combat with Ian) is not something the host ever
// decided -- confirmed via testing: the client's sprite would stand still,
// suddenly walk off-screen, then get snapped back by the next position
// sync. See coopnet_block_local_move().
bool coopnet_block_local_move(Object* owner)
{
    if (g_coopRole != CoopRole::Client || g_coopSanctionedMoveDepth > 0 || owner == NULL) {
        return false;
    }
    if (owner == g_coopCompanion) {
        coopnet_report_glitch("A script tried to move the companion by itself on the client (blocked)");
        return true;
    }
    if (owner == obj_dude) {
        debug_printf("\nCoop: note: local move of mirror obj_dude on client (tile=%d)\n", owner->tile);
        return false;
    }

    // Any other critter (Ian and other party members, townspeople with
    // wander/patrol scripts): their AI runs in the client's own independent
    // copy of the world, so left alone they walk off on their own and then
    // visibly snap back whenever something re-syncs them -- confirmed via
    // testing (Ian leaving the screen out of combat). Combat participants
    // are moved through the sanctioned path above, so they're unaffected.
    // Frozen-in-place is a better failure mode than wandering off; a real
    // fix is host-driven NPC position sync.
    if (g_coopConnState == CoopConnState::Connected && FID_TYPE(owner->fid) == OBJ_TYPE_CRITTER) {
        static int blockedLogCount = 0;
        if (blockedLogCount++ < 100) {
            debug_printf("\nCoop: BLOCKED local move of NPC pid=%d tile=%d\n", owner->pid, owner->tile);
        }
        return true;
    }
    return false;
}

// Client-side only. Call right after register_clear(obj) forcibly cancels
// an in-flight walk to splice in a one-shot attack/gesture/damage
// animation (the three COOP_MSG_*ANIM handlers below). Without this,
// coopnet_client_apply_position()'s "hasArrived" check (its own comment)
// keeps waiting forever for the object to reach g_coopLastCommandedTile --
// a walk that register_clear() just aborted mid-stride will never
// actually get there on its own, since nothing is driving it anymore.
// Confirmed via user testing as a real regression from these new
// messages: the companion visibly "ran while limping" -- stuck in place
// after every interrupted walk until it fell far enough behind to trip
// the instant-snap distance threshold. Resetting to -1 here makes the
// very next position broadcast issue a fresh walk command immediately
// instead of waiting on a stale, now-unreachable destination.
static void coopnet_client_reset_commanded_tile_for(Object* obj)
{
    if (obj == g_coopCompanion) {
        g_coopLastCommandedTile[0] = -1;
        g_coopMoveDest[0] = -1;
    } else if (obj == obj_dude) {
        g_coopLastCommandedTile[1] = -1;
        g_coopMoveDest[1] = -1;
    } else {
        // An attack/damage animation just replaced whatever this critter was
        // doing, so any replayed path is over.
        for (int i = 0; i < g_coopParticipantCount; i++) {
            if (g_coopParticipants[i].localObject == obj) {
                g_coopParticipants[i].lastCommandedTile = -1;
                g_coopParticipants[i].moveDest = -1;
                break;
            }
        }
    }
}

// Client-side only: marks a critter dead in the client's own copy of the
// world -- the real animation plays through show_damage_to_object() (which
// registers the fall + blood + show_death() flattening), but that function
// never sets the DAM_DEAD result bit itself (vanilla does that later, in
// apply_damage(), which the client never runs). Never called for the
// companion or the host's mirrored obj_dude: a dead obj_dude would trip the
// client's own local death handling, and their deaths are the host's
// GAME_OVER message's business.
static void coopnet_client_mark_dead(Object* obj)
{
    if (obj == NULL || obj == obj_dude || obj == g_coopCompanion) {
        return;
    }
    obj->data.critter.combat.results |= DAM_DEAD;
    obj->data.critter.hp = 0;
}

// Client-side only: fallback death (see the isDead branch of
// coopnet_apply_combat_participant()) for when the killing blow's own
// animation message never arrived.
static void coopnet_client_play_death(Object* obj, bool markDead)
{
    register_clear(obj);
    coopnet_client_reset_commanded_tile_for(obj);
    register_begin(ANIMATION_REQUEST_RESERVED);
    register_priority(1);
    // anim 0 = "no particular weapon animation": pick_death() then falls
    // through to the plain fall-back death.
    show_damage_to_object(obj, 0, DAM_DEAD, NULL, true, 0, 0, 0, obj, 0);
    register_end();
    if (markDead) {
        coopnet_client_mark_dead(obj);
    }
}


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
// Puts the host's final corpse pose on a critter: the dead art id AND its last
// frame (a corpse is the last frame of its death animation -- leaving frame 0
// showed the man "standing up mid-fall" for corpses that died long ago).
static void coopnet_client_place_corpse_look(Object* object, int32_t fid)
{
    if (FID_TYPE(fid) != OBJ_TYPE_CRITTER || !art_exists(fid)) {
        return;
    }
    Rect rect;
    if (fid != object->fid) {
        obj_change_fid(object, fid, &rect);
        tile_refresh_rect(&rect, object->elevation);
    }
    CacheEntry* handle;
    Art* art = art_ptr_lock(fid, &handle);
    if (art != NULL) {
        int frames = art_frame_max_frame(art);
        art_ptr_unlock(handle);
        if (frames > 0) {
            obj_set_frame(object, frames - 1, &rect);
            tile_refresh_rect(&rect, object->elevation);
        }
    }
}

// Client: streamed NPCs were moved/spawned/changed since the last full repaint.
// Animated moves and snaps repaint their own rectangles, but a missed corner
// leaves a leftover sprite ("copies of the companion that don't go away when
// the mouse passes over them") -- a full repaint at most every 1.5 s wipes them.
static bool g_coopClientNeedsRepaint = false;
static uint32_t g_coopClientLastRepaintMs = 0;

static Object* g_coopParticipantStampDude = NULL;
static int g_coopParticipantStampMap = -2;

static void coopnet_apply_combat_participant(const CoopCombatParticipant& p)
{
    if ((p.pid == 0x1000000 || p.pid == kCoopCompanionPid)) {
        return; // the player prototype is the host character / companion, never a mirrored NPC
    }
    if (!p.resync) {
        g_coopClientNeedsRepaint = true; // something moved/changed: see the periodic repaint in coopnet_poll_client()
    }

    // The table now lives for the whole map (world sync), so it must not
    // outlive a game load / map change that freed its objects.
    int stampMap = map_get_index_number();
    if (g_coopParticipantStampDude != obj_dude || g_coopParticipantStampMap != stampMap) {
        g_coopParticipantStampDude = obj_dude;
        g_coopParticipantStampMap = stampMap;
        g_coopParticipantCount = 0;
    }

    Object* object = coopnet_find_participant_object(p.id);
    bool wasSpawned = false;
    bool isNew = false;

    if (object == NULL) {
        isNew = true;
        if (g_coopParticipantCount >= kCoopMaxParticipants) {
            coopnet_report_glitch("participant table full, dropping id=%d\n", p.id);
            return;
        }

        Object* bestCandidate = NULL;
        int bestDist = kCoopParticipantAdoptDistance + 1;
        for (Object* candidate = obj_find_first_at(p.elevation); candidate != NULL; candidate = obj_find_next_at()) {
            if (candidate->pid != p.pid || FID_TYPE(candidate->fid) != OBJ_TYPE_CRITTER || (candidate->flags & OBJECT_HIDDEN) != 0) {
                continue;
            }
            if (candidate == obj_dude || candidate == g_coopCompanion) {
                continue;
            }
            if (coopnet_find_participant_id(candidate) != -1) {
                continue;
            }
            // Same map-file id: definitely the same NPC.
            if (candidate->id == p.id) {
                bestCandidate = candidate;
                break;
            }
            int dist = tile_dist(candidate->tile, p.tile);
            if (dist < bestDist) {
                bestDist = dist;
                bestCandidate = candidate;
            }
        }
        object = bestCandidate;

        // The client's own save can hold this NPC as a corpse (it was killed in
        // an earlier session on the client's side) while the host's one is
        // alive -- Ian lay on the ground on the client's screen but stood on the
        // host's. The host is right: drop that corpse and mirror a living one.
        if (object != NULL && !p.isDead && p.hp > 0 && critter_is_dead(object)) {
            debug_printf("\nCoop: client copy of id=%d (pid=%d) is a corpse but the host's is alive -- replacing it\n", p.id, p.pid);
            obj_destroy(object);
            object = NULL;
        }

        if (object == NULL) {
            if (obj_pid_new(&object, p.pid) == -1) {
                coopnet_report_glitch("obj_pid_new failed applying combat participant (pid=%d)\n", p.pid);
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
        g_coopParticipants[index].dead = false;
        g_coopParticipants[index].lastCommandedTile = -1;
        g_coopParticipants[index].moveDest = -1;
        g_coopParticipants[index].moveDestStartMs = 0;
        g_coopParticipants[index].busySinceMs = 0;
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

    if (g_coopParticipants[index].dead) {
        if (!p.isDead && p.hp > 0) {
            // The client's copy died (a stray or mis-targeted damage animation)
            // but the host's critter is alive: the host is right. Drop the
            // wrong corpse and mirror the living critter afresh.
            Object* wrong = g_coopParticipants[index].localObject;
            g_coopParticipants[index] = g_coopParticipants[g_coopParticipantCount - 1];
            g_coopParticipantCount--;
            if (wrong != NULL && wrong != obj_dude && wrong != g_coopCompanion) {
                obj_destroy(wrong);
            }
            debug_printf("\nCoop: host critter id=%d is alive but the client's copy was dead -- re-mirroring\n", p.id);
            coopnet_apply_combat_participant(p);
        }
        return;
    }

    if (p.isDead && isNew) {
        // First time this critter is seen and it is already a corpse (map
        // populated by the world stream): place it as the host's corpse, no
        // falling animation.
        Rect rect;
        if (object->tile != p.tile || object->elevation != p.elevation) {
            obj_move_to_tile(object, p.tile, p.elevation, &rect);
            tile_refresh_rect(&rect, p.elevation);
        }
        coopnet_client_place_corpse_look(object, p.fid);
        coopnet_client_mark_dead(object);
        g_coopParticipants[index].dead = true;
        return;
    }

    if (p.isDead) {
        // Leave a corpse, don't destroy: the normal path is that the
        // COOP_MSG_COMBAT_DAMAGE_ANIM for the killing blow already played
        // the real fall animation and marked the object dead. If that
        // message never made it (participant wasn't tracked yet, or the
        // kill had no show_damage_to_object() call), play a fallback fall
        // here so it still dies visibly instead of vanishing.
        debug_printf("\nCoop: participant id=%d pid=%d reported dead (hp=%d), alreadyDead=%d\n",
            p.id, p.pid, p.hp, (object->data.critter.combat.results & DAM_DEAD) != 0);
        if ((object->data.critter.combat.results & DAM_DEAD) == 0) {
            // The killing blow's own animation never arrived (yet): play a
            // generic fall. (Snapping straight to the corpse pose here
            // suppressed the fall animation whenever this message beat the
            // animation message.)
            coopnet_client_play_death(object, true);
        }
        g_coopParticipants[index].dead = true;
        return;
    }

    // p.hp was previously only ever read for the debug print above -- never
    // actually applied to the local mirror object while it's alive. Same
    // gap as the companion/obj_dude HP fix in
    // coopnet_client_apply_position()'s comment, just for enemies/allies
    // instead.
    object->data.critter.hp = p.hp;

    // A shot or a hit reaction is playing: leave the critter alone until it
    // ends. Turning it, swapping its art or snapping its tile now (all of which
    // the ~100ms position updates do) restarts the animation from its first
    // frame -- the "skipped frames" on the client when Ian fires his gun. Only
    // honoured for a couple of seconds and only while the critter is where the
    // host says it is, so a stuck animation can never freeze it out of sync.
    if (g_coopParticipants[index].moveDest == -1 && anim_busy(object)) {
        uint32_t nowMs = coopnet_now_ms();
        if (g_coopParticipants[index].busySinceMs == 0) {
            g_coopParticipants[index].busySinceMs = nowMs;
        }
        if (nowMs - g_coopParticipants[index].busySinceMs < 2500
            && object->elevation == p.elevation
            && tile_dist(object->tile, p.tile) <= kCoopSnapDistanceThreshold) {
            return;
        }
    } else {
        g_coopParticipants[index].busySinceMs = 0;
    }

    // Periodic reconciliation: the host's critter has been standing still, so
    // the client's copy must be on the same tile -- if it isn't (a missed or
    // mis-timed move), snap it there instead of trusting the animations.
    if (p.resync && !isNew && object->tile != p.tile && object->elevation == p.elevation) {
        Rect syncRect;
        register_clear(object);
        obj_move_to_tile(object, p.tile, p.elevation, &syncRect);
        obj_set_rotation(object, p.rotation, &syncRect);
        tile_refresh_rect(&syncRect, p.elevation);
        g_coopParticipants[index].lastCommandedTile = p.tile;
        return;
    }

    // Armor / weapon look, keeping this object's own animation type + facing.
    if (FID_TYPE(object->fid) == OBJ_TYPE_CRITTER && FID_TYPE(p.fid) == OBJ_TYPE_CRITTER
        && ((object->fid & 0xFFF) != (p.fid & 0xFFF) || ((object->fid & 0xF000) >> 12) != ((p.fid & 0xF000) >> 12))) {
        int newFid = art_id(OBJ_TYPE_CRITTER, p.fid & 0xFFF, FID_ANIM_TYPE(object->fid), (p.fid & 0xF000) >> 12, (object->fid & 0x70000000) >> 28);
        if (art_exists(newFid)) {
            Rect fidRect;
            obj_change_fid(object, newFid, &fidRect);
            tile_refresh_rect(&fidRect, object->elevation);
        }
    }

    // A path replayed from the host's own MOVE_ANIM is playing: let it finish
    // (the animation takes the same route and time the host's did) instead of
    // cancelling and re-aiming it on every ~100ms update. Fall through to the
    // reconciliation below once it arrives, runs far over its time, or the
    // critter has drifted too far from where the host says it is.
    if (g_coopParticipants[index].moveDest != -1) {
        // A step-limited move, or one toward a target, doesn't end on the
        // destination tile -- it's also over once the critter is back in its
        // standing pose (after a short grace so the run has really started).
        bool standingAgain = FID_ANIM_TYPE(object->fid) == ANIM_STAND
            && coopnet_now_ms() - g_coopParticipants[index].moveDestStartMs > 300;
        bool arrived = object->tile == g_coopParticipants[index].moveDest || standingAgain;
        bool timedOut = coopnet_now_ms() - g_coopParticipants[index].moveDestStartMs > kCoopMoveDestTimeoutMs;
        bool drifted = object->elevation != p.elevation || tile_dist(object->tile, p.tile) > kCoopSnapDistanceThreshold;
        if (!arrived && !timedOut && !drifted) {
            return;
        }

        g_coopParticipants[index].moveDest = -1;
        if (!arrived) {
            // The replay didn't get there: stop it and put the critter where
            // the host has it.
            Rect snapRect;
            register_clear(object);
            obj_move_to_tile(object, p.tile, p.elevation, &snapRect);
            obj_set_rotation(object, p.rotation, &snapRect);
            tile_refresh_rect(&snapRect, p.elevation);
            g_coopParticipants[index].lastCommandedTile = p.tile;
            return;
        }
        g_coopParticipants[index].lastCommandedTile = object->tile;
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

    // See coopnet_client_apply_position()'s comment on the identical
    // isInCombat() check: register_end() (in the smooth-run branch below)
    // routes through combat-turn bookkeeping the client never advances
    // when its own local simulation thinks it's in combat, silently
    // freezing the animation in place. Same fix here for participants.
    //
    // A tight tile-distance snap threshold was tried here first (>2 tiles ->
    // instant teleport, no animation) to keep an enemy's final resting tile
    // exact for clicking -- confirmed via testing this backfired badly: a
    // charging enemy covers more than 2 tiles between two ~100ms broadcasts
    // almost every update, so the whole approach became a series of
    // teleports with no run animation at all ("doesn't see the animations...
    // they often teleport"). Reverted to the same large threshold as
    // everything else; accuracy while MOVING now comes from redirecting the
    // run on every update instead (below), not from snapping.
    if (object->elevation != p.elevation || tile_dist(object->tile, p.tile) > kCoopSnapDistanceThreshold || isInCombat()) {
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
    // Outside combat (ordinary wandering), waiting for the current short run
    // to finish before redirecting is what keeps the gait smooth (see the
    // walking "limp" fix this mirrors). During a synced fight, being exactly
    // where the client can click it matters more than a perfectly smooth
    // stride -- redirect the run toward the latest reported tile on every
    // update instead of waiting, same "interrupt on new info" idea already
    // used for the companion's own moves.
    if (!hasArrived && !g_coopClientInCombat) {
        return;
    }

    bool hostRunning = FID_TYPE(p.fid) == OBJ_TYPE_CRITTER && FID_ANIM_TYPE(p.fid) == ANIM_RUNNING;
    register_clear(object);
    register_begin(ANIMATION_REQUEST_UNRESERVED);
    g_coopSanctionedMoveDepth++;
    if (g_coopClientInCombat || hostRunning) {
        register_object_run_to_tile(object, p.tile, p.elevation, -1, 0);
    } else {
        // Ordinary wandering NPCs walk.
        register_object_move_to_tile(object, p.tile, p.elevation, -1, 0);
    }
    g_coopSanctionedMoveDepth--;
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
        // A dead spawned mirror is the corpse of something the host really
        // killed -- keep it, or bodies vanish the instant the fight ends.
        if (g_coopParticipants[i].wasSpawned && !g_coopParticipants[i].dead) {
            obj_destroy(g_coopParticipants[i].localObject);
        }
    }
    g_coopParticipantCount = 0;
}

// Client-side only: COOP_MSG_WORLD_REMOVE -- the host no longer has this
// critter, so the client's copy goes too (the "NPC visible only on the client"
// ghost).
static void coopnet_apply_world_remove(const CoopWorldRemove& rem)
{
    if (g_coopParticipantStampDude != obj_dude || g_coopParticipantStampMap != map_get_index_number()) {
        return; // table belongs to an earlier map; nothing to remove
    }
    for (int i = 0; i < g_coopParticipantCount; i++) {
        if (g_coopParticipants[i].hostId == rem.id && g_coopParticipants[i].localObject != NULL
            && g_coopParticipants[i].localObject->pid == rem.pid) {
            Object* object = g_coopParticipants[i].localObject;
            g_coopParticipants[i] = g_coopParticipants[g_coopParticipantCount - 1];
            g_coopParticipantCount--;
            if (object != obj_dude && object != g_coopCompanion) {
                obj_destroy(object);
                tile_refresh_display();
            }
            return;
        }
    }
}

// Client-side only: COOP_MSG_WORLD_ITEM -- make the loose ground item exist (or
// not) at (pid, tile, elevation), like the host.
static void coopnet_apply_world_item(const CoopWorldItem& msg)
{
    if (!elevationIsValid(msg.elevation) || !hexGridTileIsValid(msg.tile)) {
        return;
    }
    Object* existing = NULL;
    for (Object* o = obj_find_first_at(msg.elevation); o != NULL; o = obj_find_next_at()) {
        if (o->tile == msg.tile && o->pid == msg.pid && FID_TYPE(o->fid) == OBJ_TYPE_ITEM && (o->flags & OBJECT_HIDDEN) == 0) {
            existing = o;
            break;
        }
    }

    if (msg.present) {
        if (existing != NULL) {
            return;
        }
        Object* item = NULL;
        if (obj_pid_new(&item, msg.pid) == -1) {
            return;
        }
        Rect rect;
        obj_move_to_tile(item, msg.tile, msg.elevation, &rect);
        tile_refresh_rect(&rect, msg.elevation);
    } else if (existing != NULL) {
        obj_destroy(existing);
        tile_refresh_display();
    }
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

    // After a game load the host's map is named "SHADYW.SAV" (the saved copy of
    // the map). The client has that saved copy only if IT loaded the same game;
    // map_load() opens a ".SAV" name as a plain file and failed when the client
    // had loaded a different save ("broke everything"). Asking for the ".MAP"
    // makes map_load use the client's own saved copy when it exists and the
    // original map otherwise.
    {
        size_t nameLen = strlen(mapName);
        if (nameLen > 4 && strcmp(mapName + nameLen - 4, ".SAV") == 0) {
            strcpy(mapName + nameLen - 4, ".MAP");
        }
    }

    debug_printf("\nCoop: client applying MAP_TRANSITION to %s (tile=%d elevation=%d sameMapElevationOnly=%d)\n", mapName, transition.tile, transition.elevation, transition.sameMapElevationOnly);

    // Elevation-only (walked into a building on the same map file, see
    // g_coopHostLastElevation's comment): the client's map is already the
    // right one and the companion object is already valid, so skip the
    // whole heavy map_load()/companion-respawn path below entirely --
    // just move both to the new elevation/tile.
    if (transition.sameMapElevationOnly) {
        if (hexGridTileIsValid(transition.tile) && elevationIsValid(transition.elevation)) {
            obj_move_to_tile(obj_dude, transition.tile, transition.elevation, NULL);
            map_set_elevation(transition.elevation);
            obj_set_rotation(obj_dude, transition.rotation, NULL);
        }

        if (tile_set_center(obj_dude->tile, TILE_SET_CENTER_REFRESH_WINDOW) == -1) {
            debug_printf("\nCoop: client attempt to center out-of-bounds after elevation-only MAP_TRANSITION\n");
        }

        if (g_coopCompanion != NULL) {
            Rect rect;
            obj_move_to_tile(g_coopCompanion, transition.tile, transition.elevation, &rect);
            tile_refresh_rect(&rect, transition.elevation);
        }

        return;
    }

    g_coopClientInCombat = false;
    g_coopClientCombatTurnActive = false;
    coopnet_clear_combat_participants();

    // The client's own local simulation can perfectly well be mid-fight when
    // this direct transition arrives (its nearby mirrored hostiles run their
    // own independent AI -- see coopnet_apply_combat_participant()'s
    // comment). Normally leaving a map through the real exit-grid/
    // map_leave_map() path tears down combat_list/list_com/list_noncom via
    // combat_over_from_load() (loadsave.cc's own save-load path does the
    // same). This bypasses that entirely, same as map_reset_transition_state()'s
    // comment just below -- so without this, combat_list kept pointing at
    // this map's critter objects, map_load() below then frees them wholesale
    // as part of tearing down the old map, and the next local fight to
    // reference one of those now-dangling slots (combat_add_noncoms()/
    // combat_end(), reading whatever object memory happens to have been
    // reused for by then -- confirmed via a crash dump landing in
    // combatai_want_to_join() on what was by then a wall object) is an
    // access violation. Tear combat down here, while its objects are still
    // the valid, about-to-be-freed old map's.
    if (isInCombat()) {
        combat_over_from_load();
    }

    // This bypasses map_leave_map()/map_check_state() entirely (it's not a
    // real in-engine exit trigger, just a direct by-name load), so
    // map_data.cc's own file-static map_state is never populated for this
    // call -- reset it first so map_load_file()'s tail code doesn't read
    // stale/garbage data. See map_reset_transition_state()'s comment.
    map_reset_transition_state();

    g_coopClientMapLoading = true;
    if (map_load(mapName) == -1) {
        g_coopClientMapLoading = false;
        coopnet_report_glitch("client failed to load map %s for MAP_TRANSITION\n", mapName);
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
    g_coopClientMapLoading = false;

    // The client's own copy of this map was populated by ITS map scripts and
    // random-encounter rolls (dead bodies, scorpions, a different crowd) --
    // none of it is what the host has. Drop every critter; the host's world
    // stream (coopnet_host_broadcast_world) re-creates the real ones.
    {
        std::vector<Object*> doomed;
        for (int elevation = 0; elevation < ELEVATION_COUNT; elevation++) {
            for (Object* o = obj_find_first_at(elevation); o != NULL; o = obj_find_next_at()) {
                if (FID_TYPE(o->fid) == OBJ_TYPE_CRITTER && o != obj_dude && o != g_coopCompanion) {
                    // Real party members (Ian, Max Stone...) are referenced by
                    // the party tables and flagged NO_REMOVE: destroying one
                    // freed memory the party code still used -- a heap
                    // corruption crash right here on entering an encounter map.
                    // They stay; the host stream adopts them by id.
                    if ((o->flags & OBJECT_NO_REMOVE) == 0 && !isPartyMember(o)) {
                        doomed.push_back(o);
                    }
                } else if (FID_TYPE(o->fid) == OBJ_TYPE_ITEM && o->tile != -1 && (o->flags & OBJECT_NO_REMOVE) == 0) {
                    doomed.push_back(o); // loose ground items too: the host streams the real ones
                }
            }
        }
        // HIDE, don't destroy: destroying these (obj_destroy) corrupted the heap
        // on random-encounter maps, whose objects the engine links in ways its
        // own map teardown copes with but a single destroy doesn't (twice, the
        // same crash dump). A hidden object neither draws nor blocks, and the
        // next map load frees it the normal way.
        for (size_t i = 0; i < doomed.size(); i++) {
            doomed[i]->flags |= (OBJECT_HIDDEN | OBJECT_NO_BLOCK);
        }
        debug_printf("\nCoop: client purged %d local critters after MAP_TRANSITION\n", static_cast<int>(doomed.size()));
        tile_refresh_display();
    }

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
    debug_printf("\nCoop-debug: dialogue-state checkpoint A (replyLen=%d optionCount=%d)\n", (int)strlen(state.replyText), state.optionCount);
    coopnet_close_dialogue_window();
    debug_printf("\nCoop-debug: dialogue-state checkpoint B (old window closed)\n");

    g_coopDialogueWin = win_add(kCoopDialogueWinX, kCoopDialogueWinY, kCoopDialogueWinWidth, kCoopDialogueWinHeight, 256, WINDOW_MOVE_ON_TOP);
    debug_printf("\nCoop-debug: dialogue-state checkpoint C (win_add returned %d)\n", g_coopDialogueWin);
    if (g_coopDialogueWin == -1) {
        return;
    }

    win_border(g_coopDialogueWin);
    debug_printf("\nCoop-debug: dialogue-state checkpoint D (bordered)\n");

    Window* window = GNW_find(g_coopDialogueWin);
    debug_printf("\nCoop-debug: dialogue-state checkpoint E (window=%p buffer=%p)\n", (void*)window, window != NULL ? (void*)window->buffer : NULL);
    unsigned char* buf = window->buffer;
    int textWidth = kCoopDialogueWinWidth - 2 * kCoopDialogueWinPadding;
    int y = kCoopDialogueWinPadding;

    // Same colorTable index gDialogProcessReply()/gDialogProcessUpdate() use
    // for the real dialogue screen's NPC line and (non-empathy-highlighted)
    // option text -- was a generic window-chrome color before, which read as
    // a plain debug box rather than something that belongs to the game's own
    // dialogue UI.
    int textColor = colorTable[992];

    if (state.replyText[0] != '\0') {
        debug_printf("\nCoop-debug: dialogue-state checkpoint F (about to draw reply text)\n");
        coopnet_dialogue_draw_wrapped(buf, kCoopDialogueWinWidth, textWidth, kCoopDialogueWinPadding, &y, state.replyText, textColor);
        debug_printf("\nCoop-debug: dialogue-state checkpoint G (drew reply text)\n");
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

        // Same "<bullet> text" convention gDialogProcessUpdate() itself
        // formats each option with (0x95 is the bullet glyph in the game's
        // own font) -- the client can't click these anyway (read-only
        // mirror), so a plain number would just imply a control the client
        // doesn't have; the bullet matches what a real player actually sees.
        debug_printf("\nCoop-debug: dialogue-state checkpoint H (option %d, len=%d)\n", i, (int)strlen(state.optionText[i]));
        char labeled[256];
        snprintf(labeled, sizeof(labeled), "%c %s", '\x95', state.optionText[i]);
        debug_printf("\nCoop-debug: dialogue-state checkpoint I (option %d formatted)\n", i);
        coopnet_dialogue_draw_wrapped(buf, kCoopDialogueWinWidth, textWidth, kCoopDialogueWinPadding, &y, labeled, textColor);
        debug_printf("\nCoop-debug: dialogue-state checkpoint J (option %d drawn)\n", i);
    }

    debug_printf("\nCoop-debug: dialogue-state checkpoint K (about to win_draw)\n");
    win_draw(g_coopDialogueWin);
    debug_printf("\nCoop-debug: dialogue-state checkpoint L (done)\n");
}

// Client-side only: a small always-on-top window shown while the host is on
// the world map screen (see COOP_MSG_WORLDMAP_BEGIN/_STATE/_END) -- same
// plain-bordered-box style and word-wrap helper as the dialogue mirror
// above, just placed lower on screen so the two never overlap on the rare
// chance both linger briefly during a fast state change.
static int g_coopWorldmapWin = -1;

const int kCoopWorldmapWinX = 10;
const int kCoopWorldmapWinY = 300;
const int kCoopWorldmapWinWidth = 300;
const int kCoopWorldmapWinHeight = 60;
const int kCoopWorldmapWinPadding = 10;

static void coopnet_close_worldmap_window()
{
    if (g_coopWorldmapWin != -1) {
        win_delete(g_coopWorldmapWin);
        g_coopWorldmapWin = -1;
    }
}

static const char* coopnet_worldmap_terrain_text(int terrain)
{
    switch (terrain) {
    case TERRAIN_TYPE_DESERT:
        return "open desert";
    case TERRAIN_TYPE_MOUNTAIN:
        return "the mountains";
    case TERRAIN_TYPE_CITY:
        return "a city";
    case TERRAIN_TYPE_COAST:
        return "the coast";
    default:
        return "the wasteland";
    }
}

// Client-side only: (re)draws the worldmap-mirror window with the host's
// current coarse travel status, creating it first if this is the first
// state received since COOP_MSG_WORLDMAP_BEGIN.
static void coopnet_client_apply_worldmap_state(const CoopWorldmapState& state)
{
    coopnet_close_worldmap_window();

    g_coopWorldmapWin = win_add(kCoopWorldmapWinX, kCoopWorldmapWinY, kCoopWorldmapWinWidth, kCoopWorldmapWinHeight, 256, WINDOW_MOVE_ON_TOP);
    if (g_coopWorldmapWin == -1) {
        return;
    }

    win_border(g_coopWorldmapWin);

    Window* window = GNW_find(g_coopWorldmapWin);
    unsigned char* buf = window->buffer;
    int textWidth = kCoopWorldmapWinWidth - 2 * kCoopWorldmapWinPadding;
    int y = kCoopWorldmapWinPadding;

    // Same colorTable index as the dialogue-mirror window above -- keeps
    // both coop overlay windows visually consistent with each other and
    // with the game's own dialogue text.
    int textColor = colorTable[992];

    char line1[64];
    snprintf(line1, sizeof(line1), "The host is %s on the world map.", state.isMoving ? "traveling through" : "standing in");
    coopnet_dialogue_draw_wrapped(buf, kCoopWorldmapWinWidth, textWidth, kCoopWorldmapWinPadding, &y, line1, textColor);

    char line2[64];
    snprintf(line2, sizeof(line2), "(%s)", coopnet_worldmap_terrain_text(state.terrain));
    coopnet_dialogue_draw_wrapped(buf, kCoopWorldmapWinWidth, textWidth, kCoopWorldmapWinPadding, &y, line2, textColor);

    win_draw(g_coopWorldmapWin);
}

// Client-side only: drives the REAL interface-bar end-turn/end-combat
// button panel and AP pips (intface.cc) instead of a custom overlay
// window -- the client's own local simulation never actually enters
// vanilla's real combat mode (see g_coopClientCombatAP's comment), so
// these never get driven by the engine's own combat code on the client at
// all; driven here instead from network state (COOP_MSG_COMBAT_BEGIN/
// _TURN/_END). Matches vanilla's own real usage pattern for these exact
// functions (see combat.cc's own combat_begin()/combat_over() and
// turn-end handling): the button panel slides open/closed with the
// overall fight, not per-turn; the buttons only light up
// (intface_end_buttons_enable()) during your own turn; the AP pips show
// real green pips during your turn or the same "not usable" all-red state
// combat.cc's own turn-end handling uses (intface_update_move_points(-1,
// -1)) otherwise. An earlier version of this drew a custom bordered
// text window instead -- replaced after the user asked for the real UI
// rather than "an obscure grey textbox".
//
// Clicking the real "end combat" button produces the exact same key code
// (13, '\r', see endCombatButton's own registration in intface.cc) as
// pressing Enter, which already routes to coopnet_on_client_end_turn()
// via game.cc's KEY_RETURN handler -- no extra click-wiring needed.
// Whether the end-turn buttons are currently lit (-1 = not known yet). Lighting
// them plays a sound, and the host sends an update for every action point the
// character spends, so doing it on every update beeped with every step.
static int g_coopClientTurnUiLit = -1;

static void coopnet_client_combat_turn_ui_begin()
{
    g_coopClientTurnUiLit = -1;
    intface_end_window_open(true);
}

static void coopnet_client_combat_turn_ui_set_active(bool active)
{
    if (active) {
        if (g_coopClientTurnUiLit != 1) {
            intface_end_buttons_enable();
            g_coopClientTurnUiLit = 1;
        }
        intface_update_move_points(g_coopClientCombatAP, 0);
    } else {
        if (g_coopClientTurnUiLit != 0) {
            intface_end_buttons_disable();
            g_coopClientTurnUiLit = 0;
        }
        intface_update_move_points(-1, -1);
    }
}

static void coopnet_client_combat_turn_ui_end()
{
    intface_end_buttons_disable();
    g_coopClientTurnUiLit = -1;
    intface_end_window_close(true);
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

        bool attemptFailed = false;
        int failCode = 0;
        bool established = false;

        if (rc > 0) {
            int soError = 0;
#ifdef _WIN32
            int soErrorLen = sizeof(soError);
#else
            socklen_t soErrorLen = sizeof(soError);
#endif
            getsockopt(g_coopPeerSocket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soError), &soErrorLen);
            if (FD_ISSET(g_coopPeerSocket, &exceptSet) || soError != 0) {
                attemptFailed = true;
                failCode = soError;
            } else if (FD_ISSET(g_coopPeerSocket, &writeSet)) {
                established = true;
            }
        } else if (coopnet_now_ms() - g_coopClientAttemptStartMs > kCoopClientAttemptTimeoutMs) {
            // A firewall silently dropping the SYN looks exactly like this:
            // no refusal, no answer, nothing -- Windows would keep waiting
            // for ~20s per attempt without ever reporting it.
            attemptFailed = true;
#ifdef _WIN32
            failCode = WSAETIMEDOUT;
#else
            failCode = ETIMEDOUT;
#endif
        }

        if (attemptFailed) {
            coopnet_client_attempt_failed(failCode, NULL);
            return;
        }

        if (established) {
            coopnet_status(COOP_STATUS_INFO, "Reached the host's PC - saying hello...");
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

    if (g_coopConnState == CoopConnState::WaitingForAck) {
        uint8_t type;
        unsigned char payload[kCoopMaxMessagePayload];
        uint16_t payloadLen;
        if (g_coopPeerClosed) {
            coopnet_client_attempt_failed(0, "the host closed the connection right after it opened");
            return;
        }
        if (coopnet_now_ms() - g_coopLastRecvTimeMs > kCoopClientAckTimeoutMs) {
            coopnet_client_attempt_failed(0, "connected, but the host's game never answered (10s)");
            return;
        }
        if (coopnet_try_recv_message(g_coopPeerSocket, &type, payload, &payloadLen)) {
            debug_printf("\nCoop: client received message type=%d len=%d while waiting for HELLO_ACK\n", type, payloadLen);
            if (type == COOP_MSG_HELLO_ACK && payloadLen == sizeof(CoopHelloAck)) {
                CoopHelloAck ack;
                memcpy(&ack, payload, sizeof(ack));
                debug_printf("\nCoop: HELLO_ACK received (accepted=%d, companionPid=%d, companionTile=%d)\n", ack.accepted, ack.companionPid, ack.companionTile);
                if (ack.accepted == 0) {
                    g_coopClientRetrying = false;
                    coopnet_status(COOP_STATUS_BAD, "The host refused the connection: you both need the exact");
                    coopnet_status(COOP_STATUS_BAD, "same fallout-ce.exe version (yours is protocol %u).", kCoopProtocolVersion);
                    coopnet_shutdown();
                    return;
                }
                if (ack.accepted != 0) {
                    g_coopClientRetrying = false;
                    coopnet_status(COOP_STATUS_GOOD, "Connected to the host!");
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

                    // The companion this client plays gets THIS machine's
                    // character, not the host's.
                    coopnet_client_send_character();
                    coopnet_client_send_blob(0);

                    // Everything the host sends next (the map transition,
                    // positions...) is handled on the NEXT poll, not this
                    // one: the co-op menu, if it's open, closes first.
                    return;
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

        coopnet_client_heap_check("start of poll (between polls)");
        coopnet_client_run_pending_sfx();
        if (g_coopClientNeedsRepaint && !g_coopClientMapLoading && !g_coopRemoteClientActive
            && coopnet_now_ms() - g_coopClientLastRepaintMs > 1500) {
            g_coopClientNeedsRepaint = false;
            g_coopClientLastRepaintMs = coopnet_now_ms();
            tile_refresh_display();
        }

        while (coopnet_try_recv_message(g_coopPeerSocket, &type, payload, &payloadLen)) {
            g_coopLastRecvTimeMs = coopnet_now_ms();

            // DEBUG (heap corruption hunt): the art-cache heap was found
            // corrupted at combat start with no clue where it happened; this
            // logs the first message after which it's bad.
            coopnet_client_heap_check("before next message");
            g_coopHeapCheckLastType = type;

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
                g_coopClientCombatAP = turn.actionPoints;
                // Keeps the companion's own object field consistent with
                // the shadow variable above, not just the on-screen pips --
                // handle_inventory()'s coop branch (inventry.cc) reads this
                // field directly (matching how it already reads obj_dude's
                // own field for the host's identical check) rather than
                // needing a separate accessor.
                if (g_coopCompanion != NULL) {
                    g_coopCompanion->data.critter.combat.ap = turn.actionPoints;
                }
                debug_printf("\nCoop: received COMBAT_TURN ap=%d, myTurnActive=%d\n", turn.actionPoints, g_coopClientCombatTurnActive);
                coopnet_client_combat_turn_ui_set_active(g_coopClientCombatTurnActive);
            } else if (type == COOP_MSG_COMBAT_BEGIN) {
                debug_printf("\nCoop: received COMBAT_BEGIN\n");
                // Defensive: guarantees a clean slate even if a previous
                // fight's COMBAT_END was somehow missed (a dropped/delayed
                // message, or a disconnect mid-fight) -- without this, any
                // participant left over from that earlier fight would keep
                // being treated as "already tracked" and never get its
                // position corrected, since a brand new fight hands out
                // fresh ids that won't match the stale entries.
                // The participant table is now the whole-world mirror table
                // (world sync) and persists across fights.
                g_coopClientInCombat = true;
                coopnet_client_combat_turn_ui_begin();
            } else if (type == COOP_MSG_COMBAT_END) {
                debug_printf("\nCoop: received COMBAT_END\n");
                g_coopClientInCombat = false;
                g_coopClientCombatTurnActive = false;
                coopnet_client_combat_turn_ui_end();
            } else if (type == COOP_MSG_COMBAT_PARTICIPANT && payloadLen == sizeof(CoopCombatParticipant)) {
                CoopCombatParticipant participant;
                memcpy(&participant, payload, sizeof(participant));
                coopnet_apply_combat_participant(participant);
            } else if (type == COOP_MSG_ATTACK_SFX && payloadLen == sizeof(CoopAttackSfx)) {
                CoopAttackSfx sfx;
                memcpy(&sfx, payload, sizeof(sfx));
                sfx.name[sizeof(sfx.name) - 1] = '\0';
                debug_printf("\nCoop: received ATTACK_SFX name=%s delay=%d\n", sfx.name, sfx.delay);
                if (sfx.name[0] != '\0' && !(sfx.delay <= 0 && coopnet_client_sfx_recently_seen(sfx.name, coopnet_now_ms()))) {
                    CoopPendingSfx pending;
                    memset(&pending, 0, sizeof(pending));
                    strncpy(pending.name, sfx.name, sizeof(pending.name) - 1);
                    // delay is in animation frames (about 100 ms each); none/negative = now
                    pending.dueMs = coopnet_now_ms() + (sfx.delay > 0 ? static_cast<uint32_t>(sfx.delay) * 60 : 0);
                    g_coopPendingSfx.push_back(pending);
                }
            } else if (type == COOP_MSG_WORLD_REMOVE && payloadLen == sizeof(CoopWorldRemove)) {
                CoopWorldRemove rem;
                memcpy(&rem, payload, sizeof(rem));
                coopnet_apply_world_remove(rem);
            } else if (type == COOP_MSG_WORLD_ITEM && payloadLen == sizeof(CoopWorldItem)) {
                CoopWorldItem worldItem;
                memcpy(&worldItem, payload, sizeof(worldItem));
                coopnet_apply_world_item(worldItem);
            } else if (type == COOP_MSG_MAP_TRANSITION && payloadLen == sizeof(CoopMapTransition)) {
                CoopMapTransition transition;
                memcpy(&transition, payload, sizeof(transition));
                if (g_coopRemoteClientActive || g_coopMovieDepth > 0) {
                    g_coopPendingTransition = transition;
                    g_coopPendingTransitionValid = true;
                } else {
                    coopnet_client_apply_map_transition(transition);
                }
            } else if (type == COOP_MSG_DIALOGUE_BEGIN) {
                char buffer[64];
                strcpy(buffer, "The host has started a conversation.");
                display_print(buffer);
            } else if (type == COOP_MSG_DIALOGUE_STATE && payloadLen == sizeof(CoopDialogueState)) {
                CoopDialogueState state;
                memcpy(&state, payload, sizeof(state));
                if (coopnet_client_dialogue_visual_active()) {
                    const char* optionTexts[kCoopMaxDialogueOptions];
                    for (int i = 0; i < state.optionCount; i++) {
                        optionTexts[i] = state.optionText[i];
                    }
                    coopnet_client_apply_dialogue_visual_state(state.replyMessageListId, state.replyMessageId, state.replyText,
                        state.optionCount, state.optionMessageListId, state.optionMessageId, state.optionReaction, optionTexts);
                } else {
                    coopnet_client_apply_dialogue_state(state);
                }
            } else if (type == COOP_MSG_REMOTE_BEGIN && payloadLen == sizeof(CoopRemoteBegin)) {
                CoopRemoteBegin remoteBegin;
                memcpy(&remoteBegin, payload, sizeof(remoteBegin));
                coopnet_remote_client_begin(remoteBegin);
            } else if (type == COOP_MSG_REMOTE_TILE && payloadLen == sizeof(CoopRemoteTile)) {
                CoopRemoteTile remoteTile;
                memcpy(&remoteTile, payload, sizeof(remoteTile));
                coopnet_remote_client_tile(remoteTile);
            } else if (type == COOP_MSG_REMOTE_PALETTE && payloadLen == sizeof(CoopRemotePalette)) {
                CoopRemotePalette remotePal;
                memcpy(&remotePal, payload, sizeof(remotePal));
                memcpy(g_coopRemotePal, remotePal.colors, sizeof(g_coopRemotePal));
                g_coopRemotePalDirty = true;
            } else if (type == COOP_MSG_REMOTE_END) {
                coopnet_remote_client_end();
            } else if (type == COOP_MSG_FLOAT_TEXT && payloadLen == sizeof(CoopFloatText)) {
                CoopFloatText floatText;
                memcpy(&floatText, payload, sizeof(floatText));
                coopnet_client_apply_float_text(floatText);
            } else if (type == COOP_MSG_DIALOGUE_DRIVER && payloadLen == 1) {
                g_coopClientDrivesDialogue = payload[0] != 0;
                if (g_coopClientDrivesDialogue) {
                    char hint[96];
                    strcpy(hint, "You are talking. Click an answer or press 1-9.");
                    display_print(hint);
                }
            } else if (type == COOP_MSG_DIALOGUE_END) {
                g_coopClientDrivesDialogue = false;
                char buffer[64];
                strcpy(buffer, "The conversation has ended.");
                display_print(buffer);
                coopnet_close_dialogue_window();
                // Defensive: the real visual system (see
                // COOP_MSG_DIALOGUE_VISUAL_END below) normally tears itself
                // down earlier/independently, but if that message was ever
                // dropped or missed, this is the outer "the conversation as
                // a whole is over" signal and a safe place to make sure it
                // isn't left stuck open. No-ops if already inactive.
                coopnet_client_end_dialogue_visual();
            } else if (type == COOP_MSG_DIALOGUE_VISUAL_BEGIN && payloadLen == sizeof(CoopDialogueVisualBegin)) {
                CoopDialogueVisualBegin visualBegin;
                memcpy(&visualBegin, payload, sizeof(visualBegin));
                debug_printf("\nCoop: received DIALOGUE_VISUAL_BEGIN (headFid=%d reaction=%d)\n", visualBegin.headFid, visualBegin.reaction);
                // The real visual screen replaces the plain-text mirror for
                // this conversation -- close it if it happened to already be
                // open (shouldn't normally be, since VISUAL_BEGIN always
                // follows the plain BEGIN for the same conversation, but
                // safe either way).
                coopnet_close_dialogue_window();
                gdialog_set_background(visualBegin.background);
                coopnet_client_begin_dialogue_visual(visualBegin.headFid, visualBegin.reaction);

                // Show who is being talked to: centre the camera on the
                // speaker and take the roof off them.
                if (hexGridTileIsValid(visualBegin.targetTile) && elevationIsValid(visualBegin.targetElevation)) {
                    obj_coop_focus_roof(visualBegin.targetTile, visualBegin.targetElevation);
                    tile_set_center(visualBegin.targetTile, TILE_SET_CENTER_REFRESH_WINDOW | TILE_SET_CENTER_FLAG_IGNORE_SCROLL_RESTRICTIONS);
                }
            } else if (type == COOP_MSG_DIALOGUE_VISUAL_END) {
                debug_printf("\nCoop: received DIALOGUE_VISUAL_END\n");
                coopnet_client_end_dialogue_visual();

                // Back to the client's own character: camera and roof.
                if (g_coopCompanion != NULL) {
                    obj_coop_focus_roof(g_coopCompanion->tile, g_coopCompanion->elevation);
                    tile_set_center(g_coopCompanion->tile, TILE_SET_CENTER_REFRESH_WINDOW | TILE_SET_CENTER_FLAG_IGNORE_SCROLL_RESTRICTIONS);
                }
            } else if (type == COOP_MSG_WORLDMAP_BEGIN) {
                char buffer[64];
                strcpy(buffer, "The host is now traveling the wasteland.");
                display_print(buffer);
            } else if (type == COOP_MSG_WORLDMAP_STATE && payloadLen == sizeof(CoopWorldmapState)) {
                CoopWorldmapState state;
                memcpy(&state, payload, sizeof(state));
                coopnet_client_apply_worldmap_state(state);
            } else if (type == COOP_MSG_WORLDMAP_END) {
                coopnet_close_worldmap_window();
            } else if (type == COOP_MSG_GAME_TIME && payloadLen == sizeof(CoopGameTime)) {
                CoopGameTime gameTime;
                memcpy(&gameTime, payload, sizeof(gameTime));
                int oldHour = game_time_hour() / 100;
                set_game_time(gameTime.gameTime);

                // Day/night ambient light is set by the map's own script
                // (map_update_p_proc), which only runs on certain triggers
                // (elevation change, pipboy close...) -- confirmed via
                // testing: the client only saw night after opening and
                // closing the pipboy. Run it whenever the hour turns over.
                if (game_time_hour() / 100 != oldHour && map_script_id != -1) {
                    scr_exec_map_update_scripts();
                }
            } else if (type == COOP_MSG_COMBAT_TEXT && payloadLen == sizeof(CoopCombatText)) {
                CoopCombatText combatText;
                memcpy(&combatText, payload, sizeof(combatText));
                char buffer[kCoopCombatTextLen];
                strncpy(buffer, combatText.text, sizeof(buffer) - 1);
                buffer[sizeof(buffer) - 1] = '\0';
                display_print(buffer);
            } else if (type == COOP_MSG_COMBAT_ATTACK_ANIM && payloadLen == sizeof(CoopCombatAttackAnim)) {
                CoopCombatAttackAnim attackAnim;
                memcpy(&attackAnim, payload, sizeof(attackAnim));
                Object* attacker = coopnet_resolve_anim_object(attackAnim.attackerId);
                if (attacker != NULL) {
                    // Face the target first: the position stream would otherwise
                    // turn the attacker in the middle of the animation, which
                    // restarts it (skipped frames).
                    if (attackAnim.facing >= 0 && attackAnim.facing < ROTATION_COUNT && attacker->rotation != attackAnim.facing) {
                        Rect faceRect;
                        obj_set_rotation(attacker, attackAnim.facing, &faceRect);
                        tile_refresh_rect(&faceRect, attacker->elevation);
                    }
                    register_clear(attacker);
                    coopnet_client_reset_commanded_tile_for(attacker);
                    register_begin(ANIMATION_REQUEST_RESERVED);
                    register_priority(1);
                    // Same sequence the host plays (actions.cc, action_ranged()): a
                    // gun is raised, fired and lowered again. Only the shot was
                    // replayed, so the raise and the lowering were missing and the
                    // gun seemed to jump in and out of the hands.
                    bool rangedShot = attackAnim.anim >= ANIM_FIRE_SINGLE && attackAnim.anim != ANIM_THROW_ANIM;
                    if (rangedShot) {
                        register_object_animate(attacker, ANIM_POINT, -1);
                    }
                    register_object_animate(attacker, attackAnim.anim, 0);
                    if (rangedShot) {
                        register_object_animate(attacker, ANIM_UNPOINT, -1);
                    }
                    register_end();
                }
            } else if (type == COOP_MSG_OBJECT_ANIM && payloadLen == sizeof(CoopCombatAttackAnim)) {
                CoopCombatAttackAnim objectAnim;
                memcpy(&objectAnim, payload, sizeof(objectAnim));
                Object* obj = coopnet_resolve_anim_object(objectAnim.attackerId);
                if (obj != NULL) {
                    register_clear(obj);
                    coopnet_client_reset_commanded_tile_for(obj);
                    register_begin(ANIMATION_REQUEST_RESERVED);
                    register_priority(1);
                    register_object_animate(obj, objectAnim.anim, 0);
                    register_end();
                }
            } else if (type == COOP_MSG_COMBAT_DAMAGE_ANIM && payloadLen == sizeof(CoopCombatDamageAnim)) {
                CoopCombatDamageAnim damageAnim;
                memcpy(&damageAnim, payload, sizeof(damageAnim));
                Object* defender = coopnet_resolve_anim_object(damageAnim.defenderId);
                if (defender != NULL) {
                    Object* attacker = coopnet_resolve_anim_object(damageAnim.attackerId);
                    if (attacker == NULL) {
                        attacker = defender;
                    }
                    register_clear(defender);
                    coopnet_client_reset_commanded_tile_for(defender);
                    register_begin(ANIMATION_REQUEST_RESERVED);
                    register_priority(1);
                    show_damage_to_object(defender, damageAnim.damage, damageAnim.flags, NULL, damageAnim.hitFromFront != 0, damageAnim.knockbackDistance, damageAnim.knockbackRotation, damageAnim.anim, attacker, damageAnim.delay);
                    register_end();
                    if ((damageAnim.flags & DAM_DEAD) != 0) {
                        coopnet_client_mark_dead(defender);
                    }
                }
            } else if (type == COOP_MSG_MOVE_ANIM && payloadLen == sizeof(CoopMoveAnim)) {
                CoopMoveAnim move;
                memcpy(&move, payload, sizeof(move));
                Object* obj = coopnet_resolve_anim_object(move.objId);
                // The host's real walk/run is replayed here for everyone, in
                // combat as well -- the client never runs combat() itself, so
                // register_end() never takes its combat-turn branch and the
                // animation just plays. (Combat movement used to be left to
                // the position updates alone, which only snapped critters to
                // each reported tile: enemies teleported instead of running.)
                if (obj != NULL && !isInCombat()
                    && obj->elevation == move.elevation && tile_dist(obj->tile, move.tile) <= 30) {
                    Object* moveTarget = move.destObjId != -1 ? coopnet_resolve_anim_object(move.destObjId) : NULL;
                    int steps = move.actionPoints != 0 ? move.actionPoints : -1;

                    register_clear(obj);
                    g_coopSanctionedMoveDepth++;
                    register_begin(ANIMATION_REQUEST_UNRESERVED);
                    if (moveTarget != NULL && moveTarget != obj) {
                        // Closing in on a target: same kind of move the host
                        // made, so it ends adjacent and after the same number
                        // of steps.
                        if (move.run) {
                            register_object_run_to_object(obj, moveTarget, steps, 0);
                        } else {
                            register_object_move_to_object(obj, moveTarget, steps, 0);
                        }
                    } else if (move.run) {
                        register_object_run_to_tile(obj, move.tile, move.elevation, steps, 0);
                    } else {
                        register_object_move_to_tile(obj, move.tile, move.elevation, steps, 0);
                    }
                    register_end();
                    g_coopSanctionedMoveDepth--;

                    if (obj == g_coopCompanion || obj == obj_dude) {
                        int which = obj == g_coopCompanion ? 0 : 1;
                        g_coopMoveDest[which] = move.tile;
                        g_coopMoveDestStartMs[which] = coopnet_now_ms();
                        g_coopLastCommandedTile[which] = move.tile;
                    } else {
                        for (int i = 0; i < g_coopParticipantCount; i++) {
                            if (g_coopParticipants[i].localObject == obj) {
                                g_coopParticipants[i].moveDest = move.tile;
                                g_coopParticipants[i].moveDestStartMs = coopnet_now_ms();
                                g_coopParticipants[i].lastCommandedTile = move.tile;
                                break;
                            }
                        }
                    }
                    debug_printf("\nCoop: client MOVE_ANIM objId=%d from=%d to=%d run=%d\n", move.objId, obj->tile, move.tile, move.run);
                }
            } else if (type == COOP_MSG_CHAR_RESTORE && payloadLen == sizeof(CoopCharBlob)) {
                CoopCharBlob restore;
                memcpy(&restore, payload, sizeof(restore));
                coopnet_client_apply_restore(restore);
            } else if (type == COOP_MSG_HEAD_FRAME && payloadLen == sizeof(CoopHeadFrame)) {
                CoopHeadFrame headFrame;
                memcpy(&headFrame, payload, sizeof(headFrame));
                coopnet_client_apply_head_frame(headFrame.fid, headFrame.frame);
            } else if (type == COOP_MSG_XP && payloadLen == sizeof(CoopXp)) {
                CoopXp xp;
                memcpy(&xp, payload, sizeof(xp));
                if (xp.amount != 0) {
                    int levelBefore = stat_pc_get(PC_STAT_LEVEL);
                    stat_pc_add_experience(xp.amount);
                    coopnet_status(COOP_STATUS_INFO, "Gained %d experience.", xp.amount);
                    if (stat_pc_get(PC_STAT_LEVEL) != levelBefore) {
                        coopnet_status(COOP_STATUS_GOOD, "You reached level %d! Open the character screen to spend your points.", stat_pc_get(PC_STAT_LEVEL));
                    }
                    coopnet_client_character_changed();
                }
            } else if (type == COOP_MSG_SETTINGS && payloadLen == sizeof(CoopSettings)) {
                CoopSettings settings;
                memcpy(&settings, payload, sizeof(settings));
                int localGame = settings.gameDifficulty;
                int localCombat = settings.combatDifficulty;
                int localSpeed = settings.combatSpeed;
                int localPlayerSpeedup = settings.playerSpeedup;
                config_get_value(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_PLAYER_SPEEDUP_KEY, &localPlayerSpeedup);
                config_get_value(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_GAME_DIFFICULTY_KEY, &localGame);
                config_get_value(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_COMBAT_DIFFICULTY_KEY, &localCombat);
                config_get_value(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_COMBAT_SPEED_KEY, &localSpeed);
                if (localGame != settings.gameDifficulty || localCombat != settings.combatDifficulty || localSpeed != settings.combatSpeed || localPlayerSpeedup != settings.playerSpeedup) {
                    config_set_value(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_PLAYER_SPEEDUP_KEY, settings.playerSpeedup);
                    config_set_value(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_GAME_DIFFICULTY_KEY, settings.gameDifficulty);
                    config_set_value(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_COMBAT_DIFFICULTY_KEY, settings.combatDifficulty);
                    config_set_value(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_COMBAT_SPEED_KEY, settings.combatSpeed);
                    coopnet_status(COOP_STATUS_INFO, "Difficulty and combat speed follow the host's settings.");
                }
            } else if (type == COOP_MSG_GVAR_DELTA && payloadLen >= 1) {
                // Variable-length: 1 count byte + count entries. Written
                // straight into the array (not via game_set_global_var(),
                // which deliberately ignores the client's own writes -- see
                // coopnet_client_ignores_local_gvar_writes()).
                int count = payload[0];
                if (payloadLen == 1 + count * static_cast<int>(sizeof(CoopGvarEntry)) && game_global_vars != NULL) {
                    for (int i = 0; i < count; i++) {
                        CoopGvarEntry entry;
                        memcpy(&entry, payload + 1 + i * sizeof(CoopGvarEntry), sizeof(entry));
                        if (entry.index >= 0 && entry.index < num_game_global_vars) {
                            game_global_vars[entry.index] = entry.value;
                        }
                    }
                }
            } else if (type == COOP_MSG_SCENERY_STATE && payloadLen == sizeof(CoopSceneryState)) {
                CoopSceneryState sceneryState;
                memcpy(&sceneryState, payload, sizeof(sceneryState));
                coopnet_apply_scenery_state(sceneryState);
            } else if (type == COOP_MSG_GAME_OVER && payloadLen == sizeof(CoopGameOver)) {
                CoopGameOver gameOver;
                memcpy(&gameOver, payload, sizeof(gameOver));
                coopnet_close_dialogue_window();
                coopnet_client_end_dialogue_visual();
                coopnet_close_worldmap_window();
                coopnet_client_combat_turn_ui_end();

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
        if (now - g_coopLastRecvTimeMs > kCoopHeartbeatTimeoutMs || g_coopPeerClosed) {
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
            coopnet_status(COOP_STATUS_BAD, "Lost connection to the host (%s).",
                g_coopPeerClosed ? "the host closed the connection or its game exited" : "no data for 2 minutes");
            coopnet_shutdown();
        }
    }
}

void coopnet_poll()
{
    // A join that's still retrying has no session (role None) between attempts.
    coopnet_client_retry_tick();

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
    g_coopHostPendingCombatActionValid = false;
    uint32_t busySinceMs = 0;

    // Last AP value actually sent, so the client's real AP pips
    // (coopnet_client_combat_turn_ui_set_active()) stay live as the
    // companion spends AP on moves/attacks during its own turn, instead of
    // just showing the value from the moment the turn started. Confirmed
    // via user testing this was missing entirely -- only the turn-start
    // and turn-end messages were ever sent.
    int lastSentAp = turn.actionPoints;

    while (true) {
        sharedFpsLimiter.mark();

        coopnet_poll();
        process_bk();

        bool companionBusy = anim_busy(companion);
        if (companionBusy) {
            if (busySinceMs == 0) {
                busySinceMs = coopnet_now_ms();
            }
        } else {
            busySinceMs = 0;
        }
        // A stuck animation must never hold the turn forever.
        bool busyTooLong = busySinceMs != 0 && coopnet_now_ms() - busySinceMs > 15000;

        if (!companionBusy && g_coopHostPendingCombatActionValid) {
            g_coopHostPendingCombatActionValid = false;
            coopnet_host_run_combat_action(g_coopHostPendingCombatAction);
        }

        // The turn ends only once the last shot or step has finished playing,
        // so what the companion just did (a kill included) is shown in full.
        if (companion->data.critter.combat.ap <= 0 && (!companionBusy || busyTooLong)) {
            break;
        }
        if (g_coopHostCombatEndTurnRequested && (!companionBusy || busyTooLong)) {
            break;
        }
        if (!coopnet_is_connected()) {
            break;
        }
        if (game_user_wants_to_quit != 0) {
            break;
        }

        if (companion->data.critter.combat.ap != lastSentAp) {
            lastSentAp = companion->data.critter.combat.ap;
            CoopCombatTurn apUpdate;
            apUpdate.actionPoints = lastSentAp;
            coopnet_send_message(g_coopPeerSocket, COOP_MSG_COMBAT_TURN, &apUpdate, sizeof(apUpdate));
        }

        renderPresent();
        sharedFpsLimiter.throttle();
    }

    g_coopHostCombatTurnActive = false;
    g_coopHostPendingCombatActionValid = false;

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

    bool running = false;
    configGetBool(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_RUNNING_KEY, &running);
    const Uint8* keys = SDL_GetKeyboardState(NULL);
    bool shift = keys[SDL_SCANCODE_LSHIFT] || keys[SDL_SCANCODE_RSHIFT];

    CoopMoveIntent intent;
    intent.targetTile = tile;
    intent.run = (running != shift) ? 1 : 0;
    bool sent = coopnet_send_message(g_coopPeerSocket, COOP_MSG_MOVE_INTENT, &intent, sizeof(intent));
    debug_printf("\nCoop: sent MOVE_INTENT targetTile=%d success=%d\n", tile, sent);
}

bool coopnet_is_companion_turn_active()
{
    return g_coopClientCombatTurnActive;
}

bool coopnet_is_client_in_synced_combat()
{
    return g_coopClientInCombat;
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

    // obj_dude is deliberately never in the synced participant table (it's
    // handled separately, via CoopPosition, see coopnet_client_apply_position()),
    // so the normal lookup below would never find it and this would
    // silently fall back to auto-target instead of the deliberate friendly
    // fire the player actually clicked on.
    int32_t targetId;
    if (target == obj_dude) {
        targetId = kCoopCombatTargetHostDude;
    } else {
        targetId = (target != NULL) ? coopnet_find_participant_id(target) : -1;
    }

    CoopCombatAction action;
    action.actionType = COOP_COMBAT_ACTION_ATTACK;
    action.targetTile = -1;
    action.targetId = targetId;
    action.targetPid = -1;
    action.hitMode = -1;
    action.hitLocation = HIT_LOCATION_UNCALLED;
    if (target != NULL) {
        // Same choice the interface bar shows: which hand, which attack, and
        // whether it is an aimed (called) shot.
        int hitMode;
        bool aiming;
        if (intface_get_attack(&hitMode, &aiming) != -1) {
            action.hitMode = hitMode;
            if (aiming && FID_TYPE(target->fid) == OBJ_TYPE_CRITTER) {
                int location;
                if (combat_pick_called_shot(target, hitMode, &location) == -1) {
                    return; // player cancelled the picker
                }
                action.hitLocation = location;
            }
        }
    }
    if (targetId == -1 && target != NULL && FID_TYPE(target->fid) == OBJ_TYPE_CRITTER) {
        action.targetTile = target->tile;
        action.targetPid = target->pid;
    }
    bool sent = coopnet_send_message(g_coopPeerSocket, COOP_MSG_COMBAT_ACTION, &action, sizeof(action));
    debug_printf("\nCoop: sent COMBAT_ACTION attack targetId=%d success=%d\n", targetId, sent);
}

void coopnet_on_client_inventory_closed()
{
    if (g_coopRole != CoopRole::Client || g_coopConnState != CoopConnState::Connected || g_coopCompanion == NULL) {
        return;
    }

    g_coopCompanionActiveHand = intface_is_item_right_hand() != 0 ? 1 : 0;
    CoopInventorySync sync;
    coopnet_build_inventory_snapshot(g_coopCompanion, sync);
    bool sent = coopnet_send_message(g_coopPeerSocket, COOP_MSG_INVENTORY_PUSH, &sync, sizeof(sync));
    debug_printf("\nCoop: sent INVENTORY_PUSH (%d items) success=%d\n", sync.itemCount, sent);
}

void coopnet_on_client_open_companion_inventory()
{
    if (g_coopConnState != CoopConnState::Connected) {
        return;
    }

    bool sent = coopnet_send_message(g_coopPeerSocket, COOP_MSG_INVENTORY_AP_REQUEST, NULL, 0);
    debug_printf("\nCoop: sent INVENTORY_AP_REQUEST success=%d\n", sent);
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

bool coopnet_client_forward_item_use(Object* user, Object* item, Object* target)
{
    if (g_coopRole != CoopRole::Client || g_coopConnState != CoopConnState::Connected
        || user == NULL || user != g_coopCompanion || item == NULL) {
        return false;
    }
    bool self = target == NULL || target == user;
    if (!self && target != obj_dude) {
        return false; // use on a third party: not forwarded (yet)
    }

    CoopUseItem req;
    req.pid = item->pid;
    req.onHostDude = self ? 0 : 1;
    bool sent = coopnet_send_message(g_coopPeerSocket, COOP_MSG_USE_ITEM, &req, sizeof(req));
    debug_printf("\nCoop: sent USE_ITEM pid=%d onHostDude=%d success=%d\n", req.pid, req.onHostDude, sent);
    return sent;
}

void coopnet_notify_screen_sfx(const char* soundName)
{
    if (g_coopRole != CoopRole::Host || g_coopConnState != CoopConnState::Connected || !g_coopRemoteHostActive || soundName == NULL) {
        return;
    }
    CoopAttackSfx msg;
    memset(&msg, 0, sizeof(msg));
    msg.delay = 0;
    strncpy(msg.name, soundName, sizeof(msg.name) - 1);
    coopnet_send_message(g_coopPeerSocket, COOP_MSG_ATTACK_SFX, &msg, sizeof(msg));
}

static Object* g_coopAttackSfxAttacker = NULL;

void coopnet_begin_attack_sfx(Object* attacker)
{
    g_coopAttackSfxAttacker = attacker;
}

void coopnet_end_attack_sfx()
{
    g_coopAttackSfxAttacker = NULL;
}

void coopnet_notify_attack_sfx(Object* owner, const char* soundName, int delay)
{
    // An attack's own sounds, and anything else the two characters do that makes
    // a sound (climbing a ladder, drawing a weapon, picking something up): the
    // client replays the movements but not their sound.
    bool attackSound = g_coopAttackSfxAttacker != NULL && owner == g_coopAttackSfxAttacker;
    bool partySound = owner != NULL && (owner == obj_dude || owner == g_coopCompanion);
    if (g_coopRole != CoopRole::Host || g_coopConnState != CoopConnState::Connected
        || (!attackSound && !partySound) || soundName == NULL) {
        return;
    }
    CoopAttackSfx msg;
    memset(&msg, 0, sizeof(msg));
    msg.delay = delay;
    strncpy(msg.name, soundName, sizeof(msg.name) - 1);
    bool sentSfx = coopnet_send_message(g_coopPeerSocket, COOP_MSG_ATTACK_SFX, &msg, sizeof(msg));
    debug_printf("\nCoop: sent ATTACK_SFX name=%s delay=%d success=%d\n", msg.name, delay, sentSfx);
}

void coopnet_on_client_loot_click(Object* critter)
{
    if (g_coopConnState != CoopConnState::Connected || critter == NULL) {
        return;
    }

    CoopItemEvent evt;
    evt.pid = critter->pid;
    evt.tile = critter->tile;
    evt.elevation = critter->elevation;
    bool sent = coopnet_send_message(g_coopPeerSocket, COOP_MSG_LOOT_REQUEST, &evt, sizeof(evt));
    debug_printf("\nCoop: sent LOOT_REQUEST pid=%d tile=%d success=%d\n", evt.pid, evt.tile, sent);
}

// Host side, from scripts.cc's looting drain: the COMPANION is looting -- the
// client operates the host's real loot screen (remote screen), trading the
// companion's inventory (obj_dude and inven_dude swapped, like client barter).
static bool coopnet_host_run_companion_screen(Object* looter, Object* container, bool steal)
{
    if (g_coopRole != CoopRole::Host || g_coopConnState != CoopConnState::Connected || looter != g_coopCompanion || looter == NULL) {
        return false;
    }

    Object* savedDude = obj_dude;
    obj_dude = looter;
    inven_set_dude(obj_dude, obj_dude->pid);
    g_coopRemoteForceDrive = true;
    coopnet_remote_begin();
    if (steal) {
        inven_steal_container(looter, container);
    } else {
        loot_container(looter, container);
    }
    coopnet_remote_end();
    g_coopRemoteForceDrive = false;
    obj_dude = savedDude;
    inven_set_dude(obj_dude, obj_dude->pid);

    g_coopCompanionActionBusy = false;
    coopnet_refresh_critter_fid(g_coopCompanion);
    coopnet_host_broadcast_companion_inventory();
    return true;
}

// ---------------------------------------------------------------------------
// Client character persistence (see coopnet_host_save_profile() in coopnet.h)
// ---------------------------------------------------------------------------

const int32_t kCoopProfileMagic = 0x504F4F43; // "COOP"
const int32_t kCoopProfileVersion = 2;

struct CoopProfileFile {
    int32_t magic;
    int32_t version;
    int32_t valid; // 0 = "no companion state" (saved without a coop host: overwrites any stale file)
    int32_t hp;
    CoopInventorySync sync;
    CoopCharBlob character; // this world's record of the client's character (blobLen 0 = none)
};

static bool g_coopHostProfileValid = false;
static CoopProfileFile g_coopHostProfile;

// Called by loadsave.cc after a save was loaded (both roles). The load frees
// every object, so the companion pointer and every mirror-table entry dangle:
// the client's HUD read a dead object ("broken hp counter") until the next map
// change. Reset them and ask for / do a fresh sync.
void coopnet_host_notify_xp(int xp)
{
    if (g_coopRole != CoopRole::Host || g_coopConnState != CoopConnState::Connected || xp == 0) {
        return;
    }
    CoopXp msg;
    msg.amount = xp;
    coopnet_send_message(g_coopPeerSocket, COOP_MSG_XP, &msg, sizeof(msg));
    if (xp > 0) {
        g_coopHostXpGranted += xp;
    }
}

void coopnet_on_character_screen_closed()
{
    if (g_coopRole == CoopRole::Client && g_coopConnState == CoopConnState::Connected) {
        coopnet_client_character_changed();
    }
}

void coopnet_on_game_reset()
{
    if (g_coopCompanion != NULL) {
        coopnet_destroy_companion(g_coopCompanion);
        g_coopCompanion = NULL;
    }

    // Everything mirrored from the old world goes with it.
    g_coopParticipantCount = 0;
    g_coopParticipantStampDude = NULL;
}

void coopnet_on_game_loaded()
{
    g_coopParticipantCount = 0;
    g_coopParticipantStampDude = NULL;
    g_coopPendingSfx.clear();
    g_coopLastCommandedTile[0] = -1;
    g_coopLastCommandedTile[1] = -1;
    g_coopMoveDest[0] = -1;
    g_coopMoveDest[1] = -1;
    g_coopClientNeedsRepaint = false;

    // A new game state: a death that already ended the previous game must not
    // keep switching off the "someone died -> game over" check (after
    // reloading, a second death just left the body lying there).
    g_coopCompanionGameOverSent = false;

    // A full Load Game (unlike a map transition) wipes the whole map's
    // objects via map_load_file()'s own obj_remove_all() -- which, exactly
    // like coopnet_destroy_companion()'s comment describes for transitions,
    // silently refuses to touch the companion because of its own
    // OBJECT_NO_REMOVE flag. Dropping the reference here without destroying
    // it first (the bug: just `g_coopCompanion = NULL`) left that old
    // object fully intact and still on the map, then coopnet_find_or_spawn_companion()
    // below -- unable to find it, since it's also OBJECT_NO_SAVE and so was
    // never in the save file to "find" -- spawned a fresh one right next to
    // it. Confirmed via testing: reloading a save N times left N leftover
    // companion copies standing around. Destroy the old one first, same as
    // the transition path already does.
    if (g_coopRole == CoopRole::Client) {
        coopnet_destroy_companion(g_coopCompanion);
        g_coopCompanion = NULL;
        if (g_coopConnState == CoopConnState::Connected) {
            coopnet_send_message(g_coopPeerSocket, COOP_MSG_RESYNC_REQUEST, NULL, 0);
            // A different save can mean a different character.
            coopnet_client_send_character();
        }
    } else if (g_coopRole == CoopRole::Host) {
        coopnet_destroy_companion(g_coopCompanion);
        g_coopCompanion = NULL;
        coopnet_host_reset_world_shadow();
        g_coopCompanion = coopnet_find_or_spawn_companion(obj_dude->pid, obj_dude->tile, obj_dude->elevation);
        coopnet_host_apply_saved_profile();
        coopnet_host_push_character_record();
        g_coopHostLastMapName[0] = '\0'; // makes the next tick send the client a MAP_TRANSITION
    }
}

void coopnet_host_save_profile(const char* path)
{
    CoopProfileFile file;
    memset(&file, 0, sizeof(file));
    file.magic = kCoopProfileMagic;
    file.version = kCoopProfileVersion;

    if (g_coopRole == CoopRole::Host && g_coopCompanion != NULL) {
        file.valid = 1;
        file.hp = g_coopCompanion->data.critter.hp;
        coopnet_build_inventory_snapshot(g_coopCompanion, file.sync);
    }

    // The character record belongs to the host's save, hosting or not.
    if (g_coopHostCharRecord.blobLen > 0) {
        file.valid = 1;
        file.character = g_coopHostCharRecord;
    }

    // The SAVEGAME directory may not exist yet on a save slot's very first
    // write (a fresh game the player has never manually saved) -- same
    // mkdir SaveSlot() (loadsave.cc) does for a real save, done here too
    // since this can now be the very first thing written to disk (autosave).
    char* masterPatchesPath;
    if (config_get_string(&game_config, GAME_CONFIG_SYSTEM_KEY, GAME_CONFIG_MASTER_PATCHES_KEY, &masterPatchesPath)) {
        char dirPath[64];
        snprintf(dirPath, sizeof(dirPath), "%s\\%s", masterPatchesPath, "SAVEGAME");
        compat_mkdir(dirPath);
    }

    DB_FILE* stream = db_fopen(path, "wb");
    if (stream == NULL) {
        coopnet_report_glitch("could not write client profile %s\n", path);
        return;
    }
    db_fwrite(&file, sizeof(file), 1, stream);
    db_fclose(stream);
    debug_printf("\nCoop: saved client profile %s (valid=%d hp=%d items=%d)\n", path, file.valid, file.hp, file.sync.itemCount);
}

// Read one profile file; true (and g_coopHostProfile set) if it holds a character.
static bool coopnet_host_read_profile(const char* path)
{
    DB_FILE* stream = db_fopen(path, "rb");
    if (stream == NULL) {
        return false;
    }
    CoopProfileFile file;
    size_t got = db_fread(&file, sizeof(file), 1, stream);
    db_fclose(stream);

    if (got != 1 || file.magic != kCoopProfileMagic || file.version != kCoopProfileVersion || file.valid == 0) {
        return false;
    }
    g_coopHostProfile = file;
    g_coopHostProfileValid = true;
    g_coopHostCharRecord = file.character;
    debug_printf("\nCoop: loaded client profile %s (hp=%d items=%d)\n", path, file.hp, file.sync.itemCount);
    return true;
}

// The client character is also written to this file every 30 seconds while
// hosting and the moment the client disconnects, so a crash or a dropped
// connection never loses it. It is only the fallback for a save slot that has
// no COOP.DAT of its own (a slot's own file always wins, so loading an older
// save rolls the client back with the host, like single player).
static const char* const kCoopAutosavePath = "SAVEGAME\\COOP_AUTO.DAT";

void coopnet_host_load_profile(const char* path)
{
    g_coopHostProfileValid = false;
    // A save without a record of the client's character starts with none --
    // loading an older save rolls the client back with the host.
    memset(&g_coopHostCharRecord, 0, sizeof(g_coopHostCharRecord));
    if (!coopnet_host_read_profile(path)) {
        coopnet_host_read_profile(kCoopAutosavePath);
        // The autosave is only a stand-in for the client's hp and items. Its
        // character record is the LATEST state, not this save's: using it would
        // let loading an older save keep everything the client earned since.
        memset(&g_coopHostCharRecord, 0, sizeof(g_coopHostCharRecord));
    }
}

// Host, every poll tick.
static void coopnet_host_autosave_profile()
{
    if (g_coopRole != CoopRole::Host || g_coopCompanion == NULL) {
        return;
    }
    uint32_t now = coopnet_now_ms();
    if (now - g_coopLastProfileAutosaveMs < 30000) {
        return;
    }
    g_coopLastProfileAutosaveMs = now;
    coopnet_host_save_profile(kCoopAutosavePath);
}

// Called when hosting starts (the companion has just been created).
static void coopnet_host_apply_saved_profile()
{
    if (!g_coopHostProfileValid || g_coopCompanion == NULL) {
        return;
    }
    g_coopHostProfileValid = false;

    g_coopCompanionActiveHand = g_coopHostProfile.sync.activeHand != 0 ? 1 : 0;
    coopnet_apply_companion_inventory(g_coopHostProfile.sync);
    if (g_coopHostProfile.hp > 0) {
        g_coopCompanion->data.critter.hp = g_coopHostProfile.hp;
    }
    coopnet_refresh_critter_fid(g_coopCompanion);
    debug_printf("\nCoop: applied saved client profile (hp=%d items=%d)\n", g_coopHostProfile.hp, g_coopHostProfile.sync.itemCount);
}

bool coopnet_host_run_companion_loot(Object* looter, Object* container)
{
    return coopnet_host_run_companion_screen(looter, container, false);
}

// Stealing: same driven screen as looting (the client sees and operates the
// host's real steal window; the host watches), replacing the old auto-resolve.
bool coopnet_host_run_companion_steal(Object* thief, Object* target)
{
    return coopnet_host_run_companion_screen(thief, target, true);
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

// Host-side only. Wrap a combat_display(attack) call with these two
// whenever attack->attacker or attack->defender is the companion, so the
// client sees the exact same real combat text the host does (damage
// amounts, critical hits, misses, death -- whatever combat_display()
// itself decides to print, verbatim) instead of the generic "Companion
// takes damage!" placeholder that used to be the only feedback on the
// client's screen. See coopnet_capture_display_print()'s comment for the
// actual capture mechanism (a single hook in display_print(), display.cc).
void coopnet_begin_capture_combat_text()
{
    g_coopCapturingCombatText = true;
}

void coopnet_end_capture_combat_text()
{
    g_coopCapturingCombatText = false;
}

// Called unconditionally from display_print() (display.cc) -- cheap no-op
// check when not currently capturing (the overwhelming majority of the
// thousands of display_print() calls across the whole codebase), only
// does real work during the narrow combat_display() window above.
void coopnet_capture_display_print(const char* text)
{
    if (!g_coopCapturingCombatText) {
        return;
    }

    if (g_coopRole != CoopRole::Host || g_coopConnState != CoopConnState::Connected) {
        return;
    }

    if (text == NULL || text[0] == '\0') {
        return;
    }

    CoopCombatText msg;
    strncpy(msg.text, text, sizeof(msg.text) - 1);
    msg.text[sizeof(msg.text) - 1] = '\0';

    bool sent = coopnet_send_message(g_coopPeerSocket, COOP_MSG_COMBAT_TEXT, &msg, sizeof(msg));
    debug_printf("\nCoop: captured combat text \"%.60s\" success=%d\n", msg.text, sent);
}

// Host-side: identifies an object for the attack/damage animation messages
// below. Only companion and obj_dude resolve to something real for now
// (v1 scope, see CoopCombatDamageAnim's comment) -- everything else,
// including tracked participants, comes back -1 and the caller skips
// sending rather than describing an animation the client has no local
// object to play it on.
static int32_t coopnet_resolve_anim_id(Object* obj, bool anyCritter = false)
{
    if (obj == NULL) {
        return -1;
    }
    if (obj == g_coopCompanion) {
        return kCoopAnimIdCompanion;
    }
    if (obj == obj_dude) {
        return kCoopCombatTargetHostDude;
    }

    // Any critter the world stream mirrors is tracked on the client by
    // Object::id, so its attack/damage/death animations can be mirrored too
    // (a critter the client doesn't know simply resolves to nothing there and
    // is skipped). `anyCritter` is for those one-shot animations: they used to
    // be limited to the fight's active combat list, so an NPC the client
    // attacked that hadn't joined the list yet got no damage animation at all
    // -- confirmed via a test log: the client fell back to a generic fall and
    // its death looked different from the host's.
    if (anyCritter && FID_TYPE(obj->fid) == OBJ_TYPE_CRITTER && obj->id != -1) {
        return obj->id;
    }

    // Movement stays limited to the fight's combat list: replaying every
    // wandering NPC's walk would be a flood the position stream already covers.
    if (isInCombat() && FID_TYPE(obj->fid) == OBJ_TYPE_CRITTER) {
        int count = combat_get_list_count();
        for (int i = 0; i < count; i++) {
            if (combat_get_list_item(i) == obj) {
                return obj->id;
            }
        }
    }
    return -1;
}

// Host-side only. Call from action_attack()'s own top (actions.cc) with
// the attacker and the anim code it already computed (item_w_anim) --
// mirrors the attacker's real swing/point/fire animation to the client
// whenever the attacker is the companion or obj_dude.
void coopnet_notify_attack_anim(Object* attacker, int anim, Object* defender)
{
    if (g_coopRole != CoopRole::Host || g_coopConnState != CoopConnState::Connected) {
        return;
    }

    int32_t attackerId = coopnet_resolve_anim_id(attacker, true);
    if (attackerId == -1) {
        return;
    }

    CoopCombatAttackAnim msg;
    msg.attackerId = attackerId;
    msg.anim = anim;
    msg.facing = (attacker != NULL && defender != NULL && attacker != defender) ? tile_dir(attacker->tile, defender->tile) : -1;

    bool sent = coopnet_send_message(g_coopPeerSocket, COOP_MSG_COMBAT_ATTACK_ANIM, &msg, sizeof(msg));
    debug_printf("\nCoop: notified attack anim attackerId=%d anim=%d success=%d\n", attackerId, anim, sent);
}

// Host-side only. Same idea and wire shape as coopnet_notify_attack_anim()
// above, for non-combat one-shot gestures: call with the object and anim
// code right where a_use_obj()/action_get_an_object() (actions.cc) compute
// them, before their own register_object_animate() call. No-op unless
// obj is the companion or obj_dude.
void coopnet_notify_object_anim(Object* obj, int anim)
{
    if (g_coopRole != CoopRole::Host || g_coopConnState != CoopConnState::Connected) {
        return;
    }

    int32_t objId = coopnet_resolve_anim_id(obj, true);
    if (objId == -1) {
        return;
    }

    CoopCombatAttackAnim msg;
    msg.attackerId = objId;
    msg.anim = anim;
    msg.facing = -1;

    bool sent = coopnet_send_message(g_coopPeerSocket, COOP_MSG_OBJECT_ANIM, &msg, sizeof(msg));
    debug_printf("\nCoop: notified object anim objId=%d anim=%d success=%d\n", objId, anim, sent);
}

// Host-side only. Call from register_object_move_to_tile()/
// register_object_run_to_tile() (anim.cc) once the move is accepted.
// No-op unless obj is the companion or obj_dude.
void coopnet_notify_move(Object* obj, int tile, int elevation, bool run, int actionPoints)
{
    if (g_coopRole != CoopRole::Host || g_coopConnState != CoopConnState::Connected) {
        return;
    }

    int32_t objId = coopnet_resolve_anim_id(obj);
    if (objId == -1) {
        return;
    }

    CoopMoveAnim msg;
    msg.objId = objId;
    msg.tile = tile;
    msg.elevation = elevation;
    msg.destObjId = -1;
    msg.actionPoints = actionPoints;
    msg.run = run ? 1 : 0;
    coopnet_send_message(g_coopPeerSocket, COOP_MSG_MOVE_ANIM, &msg, sizeof(msg));
    debug_printf("\nCoop: notified move objId=%d tile=%d run=%d ap=%d\n", objId, tile, msg.run, actionPoints);
}

void coopnet_notify_move_to_object(Object* obj, Object* destination, bool run, int actionPoints)
{
    if (g_coopRole != CoopRole::Host || g_coopConnState != CoopConnState::Connected || destination == NULL) {
        return;
    }

    int32_t objId = coopnet_resolve_anim_id(obj);
    if (objId == -1) {
        return;
    }

    CoopMoveAnim msg;
    msg.objId = objId;
    msg.tile = destination->tile;
    msg.elevation = destination->elevation;
    // Only a target the client can find (companion, host character or a synced
    // fight participant) can be moved toward; anything else falls back to
    // running at the tile it stands on.
    msg.destObjId = coopnet_resolve_anim_id(destination);
    msg.actionPoints = actionPoints;
    msg.run = run ? 1 : 0;
    coopnet_send_message(g_coopPeerSocket, COOP_MSG_MOVE_ANIM, &msg, sizeof(msg));
    debug_printf("\nCoop: notified move-to-object objId=%d dest=%d tile=%d run=%d ap=%d\n", objId, msg.destObjId, msg.tile, msg.run, actionPoints);
}

// Host-side only. Call from obj_use_door()'s own top (protinst.cc) with
// the door object and the state it's about to become (its frame/openFlags
// branch is already decided by that point). Identifies the door by
// (pid, tile, elevation) rather than a companion/obj_dude id -- any door
// can be opened, not just ones the companion personally touches (obj_dude
// too, or in principle any critter, though only companion/obj_dude
// interactions are host-driven-and-relevant here).
void coopnet_notify_scenery_state(Object* scenery, bool isOpen)
{
    if (g_coopRole != CoopRole::Host || g_coopConnState != CoopConnState::Connected) {
        return;
    }

    if (scenery == NULL) {
        return;
    }

    CoopSceneryState msg;
    msg.pid = scenery->pid;
    msg.tile = scenery->tile;
    msg.elevation = scenery->elevation;
    msg.isOpen = isOpen ? 1 : 0;

    bool sent = coopnet_send_message(g_coopPeerSocket, COOP_MSG_SCENERY_STATE, &msg, sizeof(msg));
    debug_printf("\nCoop: notified scenery state pid=%d tile=%d isOpen=%d success=%d\n", msg.pid, msg.tile, msg.isOpen, sent);
}

// Host-side only. Call from show_damage_to_object()'s own top (actions.cc)
// with its exact same parameters -- mirrors that one real damage-reaction
// animation (hit/knockback/death, whatever it decides to play) to the
// client whenever `defender` (the object show_damage_to_object is about to
// animate) is the companion or obj_dude. See CoopCombatDamageAnim's
// comment for why `weapon` is dropped and `attacker` falls back to
// `defender` on the receiving end when it can't be resolved.
void coopnet_notify_damage_anim(Object* defender, int damage, int flags, bool hitFromFront, int knockbackDistance, int knockbackRotation, int anim, Object* attacker, int delay)
{
    if (g_coopRole != CoopRole::Host || g_coopConnState != CoopConnState::Connected) {
        return;
    }

    int32_t defenderId = coopnet_resolve_anim_id(defender, true);
    if (defenderId == -1) {
        return;
    }

    CoopCombatDamageAnim msg;
    msg.attackerId = coopnet_resolve_anim_id(attacker, true);
    msg.defenderId = defenderId;
    msg.damage = damage;
    msg.flags = flags;
    msg.knockbackDistance = knockbackDistance;
    msg.knockbackRotation = knockbackRotation;
    msg.anim = anim;
    msg.delay = delay;
    msg.hitFromFront = hitFromFront ? 1 : 0;

    bool sent = coopnet_send_message(g_coopPeerSocket, COOP_MSG_COMBAT_DAMAGE_ANIM, &msg, sizeof(msg));
    debug_printf("\nCoop: notified damage anim defenderId=%d flags=%d anim=%d success=%d\n", defenderId, flags, anim, sent);
}

// True on a connected client: its own writes to global variables must be
// ignored, because the host's copy is the only authoritative one (quests,
// karma and story flags are whatever the host says they are). Without this
// the client's independently-running scripts changed variables the host never
// saw, and the two worlds disagreed about quest state.
// Client's own NPC brains (critter/spatial/combat script procs) stay off: the
// host is the one simulation and streams every NPC (world sync).
bool coopnet_client_freezes_local_scripts()
{
    return g_coopRole == CoopRole::Client && g_coopConnState == CoopConnState::Connected;
}

bool coopnet_client_ignores_local_gvar_writes()
{
    return g_coopRole == CoopRole::Client && g_coopConnState == CoopConnState::Connected;
}

// Client-side only. Call when a pipboy rest finishes (pipboy.cc's
// TimedRest()) -- see COOP_MSG_TIME_ADVANCE.
void coopnet_on_client_rest_finished()
{
    if (g_coopRole != CoopRole::Client || g_coopConnState != CoopConnState::Connected) {
        return;
    }

    CoopGameTime msg;
    msg.gameTime = game_time();
    coopnet_send_message(g_coopPeerSocket, COOP_MSG_TIME_ADVANCE, &msg, sizeof(msg));
    debug_printf("\nCoop: sent TIME_ADVANCE gameTime=%d\n", msg.gameTime);
}

// Nonzero while the client is applying a floating text the host sent -- see
// coopnet_client_blocks_local_float_text().
static int g_coopSanctionedTextDepth = 0;

// True on a connected client for any floating text that did NOT come from the
// host: the client's own independent scripts/AI would otherwise make NPCs bark
// at unrelated moments (a second, unsynced set of speech bubbles).
bool coopnet_client_blocks_local_float_text()
{
    return g_coopRole == CoopRole::Client && g_coopConnState == CoopConnState::Connected && g_coopSanctionedTextDepth == 0;
}

// Host-side only. Call from text_object_create() (textobj.cc), the single
// function every floating message goes through.
void coopnet_notify_float_text(Object* obj, const char* text, int font, int color, int a5)
{
    if (g_coopRole != CoopRole::Host || g_coopConnState != CoopConnState::Connected || obj == NULL || text == NULL || text[0] == '\0') {
        return;
    }

    CoopFloatText msg;
    memset(&msg, 0, sizeof(msg));
    msg.animId = coopnet_resolve_anim_id(obj);
    msg.pid = obj->pid;
    msg.tile = obj->tile;
    msg.elevation = obj->elevation;
    msg.font = font;
    msg.color = color;
    msg.a5 = a5;
    strncpy(msg.text, text, sizeof(msg.text) - 1);
    coopnet_send_message(g_coopPeerSocket, COOP_MSG_FLOAT_TEXT, &msg, sizeof(msg));
}

// Client-side only: shows one host floating text over the local mirror of the
// object it belongs to.
static void coopnet_client_apply_float_text(const CoopFloatText& f)
{
    Object* obj = f.animId != -1 ? coopnet_resolve_anim_object(f.animId) : NULL;

    if (obj == NULL) {
        int bestDist = kCoopParticipantAdoptDistance + 1;
        for (Object* candidate = obj_find_first_at(f.elevation); candidate != NULL; candidate = obj_find_next_at()) {
            if (candidate->pid != f.pid) {
                continue;
            }
            int dist = tile_dist(candidate->tile, f.tile);
            if (dist < bestDist) {
                bestDist = dist;
                obj = candidate;
            }
        }
    }

    if (obj == NULL) {
        return;
    }

    char buffer[kCoopFloatTextLen];
    strncpy(buffer, f.text, sizeof(buffer) - 1);
    buffer[sizeof(buffer) - 1] = '\0';

    g_coopSanctionedTextDepth++;
    Rect rect;
    if (text_object_create(obj, buffer, f.font, f.color, f.a5, &rect) != -1) {
        tile_refresh_rect(&rect, obj->elevation);
    }
    g_coopSanctionedTextDepth--;
}

// Client-side only: the player clicked "talk" on an NPC. Sends the request;
// the host does everything else (see coopnet_host_apply_dialogue_start()).
void coopnet_on_client_talk_click(Object* target)
{
    if (g_coopRole != CoopRole::Client || g_coopConnState != CoopConnState::Connected || target == NULL) {
        return;
    }

    CoopItemEvent evt;
    evt.pid = target->pid;
    evt.tile = target->tile;
    evt.elevation = target->elevation;
    bool sent = coopnet_send_message(g_coopPeerSocket, COOP_MSG_DIALOGUE_START_REQUEST, &evt, sizeof(evt));
    debug_printf("\nCoop: sent DIALOGUE_START_REQUEST pid=%d tile=%d success=%d\n", evt.pid, evt.tile, sent);
}

// Host-side only: applies a COOP_MSG_DIALOGUE_START_REQUEST. Finds the NPC
// (nearest matching critter -- NPC tiles differ between the two worlds, same
// reasoning as coopnet_host_apply_combat_start()) and queues the real
// conversation; the client that asked drives it.
static void coopnet_host_apply_dialogue_start(const CoopItemEvent& evt)
{
    if (g_coopCompanion == NULL || isInCombat() || dialog_active()) {
        return;
    }

    Object* target = NULL;
    int bestDist = kCoopParticipantAdoptDistance + 1;
    for (Object* object = obj_find_first_at(evt.elevation); object != NULL; object = obj_find_next_at()) {
        if (object->pid != evt.pid
            || FID_TYPE(object->fid) != OBJ_TYPE_CRITTER
            || object == obj_dude || object == g_coopCompanion
            || critter_is_dead(object)) {
            continue;
        }
        int dist = tile_dist(object->tile, evt.tile);
        if (dist < bestDist) {
            bestDist = dist;
            target = object;
        }
    }

    debug_printf("\nCoop: client requested dialogue, target=%p (pid=%d)\n", (void*)target, evt.pid);
    if (target == NULL) {
        return;
    }

    // The companion walks over to the NPC first (runs if far), then the
    // conversation starts -- the driver flag is raised on arrival, see
    // coopnet_note_companion_reached_npc(). Clears any move the client had going.
    g_coopLastCommandedTile[0] = -1;
    if (action_talk_to(g_coopCompanion, target) == -1) {
        coopnet_report_glitch("companion could not start walking to talk to pid=%d\n", evt.pid);
    }
}

void coopnet_note_companion_reached_npc()
{
    g_coopDialogueDriverPending = true;
    g_coopDialogueDriverPendingMs = coopnet_now_ms();
}

// Host-side: called from talk_to() (actions.cc) when the host's own
// character (obj_dude, not the companion) is the one whose Talk click is
// opening this conversation. See g_coopDialogueHostInitiated's comment.
void coopnet_mark_dialogue_host_initiated()
{
    g_coopDialogueHostInitiated = true;
}

// Host-side: an NPC's own script is starting a conversation (its
// dialogue_system_enter opcode) -- e.g. someone who talks to whoever walks up
// to them. If the client's companion is the one that's nearer to that NPC,
// the client is the one being spoken to and should drive it; without this the
// host got the conversation (and answered for the client) whenever the client
// was the one who walked up to the NPC -- confirmed via testing.
void coopnet_mark_dialogue_client_initiated(Object* npc)
{
    if (g_coopRole != CoopRole::Host || g_coopConnState != CoopConnState::Connected
        || g_coopCompanion == NULL || npc == NULL) {
        return;
    }

    // The host's own Talk click already tells us for certain who's driving --
    // never let the proximity guess below override that. See
    // g_coopDialogueHostInitiated's comment.
    if (g_coopDialogueHostInitiated) {
        return;
    }

    if (obj_dist(npc, g_coopCompanion) < obj_dist(npc, obj_dude)) {
        g_coopDialogueDriverPending = true;
        g_coopDialogueDriverPendingMs = coopnet_now_ms();
        debug_printf("\nCoop: NPC pid=%d started a conversation with the companion (nearer than the host) -- client will drive\n", npc->pid);
    }
}

// True while a conversation is (about to be) driven by the client -- used by
// gdialog.cc (skip the host-position "can you see them" check, ignore the
// host's own input) and by op_dude_obj() (the speaker is the companion).
bool coopnet_dialogue_driven_by_client()
{
    if (g_coopDialogueDrivenByClient) {
        return true;
    }
    return g_coopDialogueDriverPending
        && coopnet_now_ms() - g_coopDialogueDriverPendingMs < kCoopDialogueDriverPendingTimeoutMs;
}

// Host-side, called from gDialogProcess()'s loop with the key it just read
// from local input. While the client drives, the host's own keyboard/mouse is
// ignored (it is watching) and the client's pick, if one arrived, is
// returned instead as the digit key the loop already treats as "choose option
// N". Quit keys still get through.
int coopnet_dialogue_filter_input(int keyCode)
{
    if (!g_coopDialogueDrivenByClient) {
        return keyCode;
    }

    if (g_coopPendingDialoguePick >= 0) {
        int pick = g_coopPendingDialoguePick;
        g_coopPendingDialoguePick = -1;
        return 49 + pick;
    }
    if (g_coopPendingDialoguePick == -2 || g_coopPendingDialoguePick == -3) {
        // Driver commands: the same keys the loop already maps to the
        // barter / "tell me about" buttons.
        int command = g_coopPendingDialoguePick;
        g_coopPendingDialoguePick = -1;
        return command == -2 ? KEY_LOWERCASE_B : KEY_LOWERCASE_A;
    }

    if (keyCode == KEY_CTRL_Q || keyCode == KEY_CTRL_X || keyCode == KEY_F10) {
        return keyCode;
    }
    return -1;
}

// Client-side: a dialogue command button (barter / "tell me about") on the
// puppet window. Only meaningful while this client drives the conversation.
// Encoded as a negative PICK value: -2 barter, -3 tell-me-about.
void coopnet_on_client_dialogue_command(int command)
{
    if (g_coopRole != CoopRole::Client || g_coopConnState != CoopConnState::Connected || !g_coopClientDrivesDialogue) {
        return;
    }
    int32_t pick = command == 2 ? -2 : -3;
    coopnet_send_message(g_coopPeerSocket, COOP_MSG_DIALOGUE_PICK, &pick, sizeof(pick));
    debug_printf("\nCoop: sent dialogue command %d\n", command);
}

// Client-side, called from game_handle_input() with every key event: while
// this client drives a conversation, digit keys 1-9 (and the mouse click on
// an option button, which produces the same key code) pick that option and
// are sent to the host. Returns true if the key was consumed.
bool coopnet_on_client_dialogue_key(int keyCode)
{
    if (g_coopRole != CoopRole::Client || g_coopConnState != CoopConnState::Connected || !g_coopClientDrivesDialogue) {
        return false;
    }
    if (keyCode < 49 || keyCode > 57) {
        return false;
    }

    int32_t pick = keyCode - 49;
    bool sent = coopnet_send_message(g_coopPeerSocket, COOP_MSG_DIALOGUE_PICK, &pick, sizeof(pick));
    debug_printf("\nCoop: sent DIALOGUE_PICK option=%d success=%d\n", pick, sent);
    return true;
}

void coopnet_notify_dialogue_begin()
{
    if (g_coopRole != CoopRole::Host || g_coopConnState != CoopConnState::Connected) {
        return;
    }

    // Promote a queued client request to "this conversation is client-driven".
    g_coopDialogueDrivenByClient = coopnet_dialogue_driven_by_client();
    g_coopDialogueDriverPending = false;
    g_coopDialogueHostInitiated = false;
    g_coopPendingDialoguePick = -1;

    bool sent = coopnet_send_message(g_coopPeerSocket, COOP_MSG_DIALOGUE_BEGIN, NULL, 0);
    debug_printf("\nCoop: notified peer dialogue began (driver=%s), success=%d\n", g_coopDialogueDrivenByClient ? "client" : "host", sent);

    uint8_t driverIsClient = g_coopDialogueDrivenByClient ? 1 : 0;
    coopnet_send_message(g_coopPeerSocket, COOP_MSG_DIALOGUE_DRIVER, &driverIsClient, sizeof(driverIsClient));
}

void coopnet_notify_dialogue_end()
{
    if (g_coopDialogueDrivenByClient) {
        g_coopTravelAfterClientDialogueMs = coopnet_now_ms();
    }
    g_coopDialogueDrivenByClient = false;
    g_coopDialogueDriverPending = false;
    g_coopDialogueHostInitiated = false;
    g_coopPendingDialoguePick = -1;

    if (g_coopRole != CoopRole::Host || g_coopConnState != CoopConnState::Connected) {
        return;
    }

    bool sent = coopnet_send_message(g_coopPeerSocket, COOP_MSG_DIALOGUE_END, NULL, 0);
    debug_printf("\nCoop: notified peer dialogue ended, success=%d\n", sent);
}

void coopnet_notify_dialogue_state(int replyListId, int replyMsgId, const char* replyText,
    const int* optionListIds, const int* optionMsgIds, const int* optionReactions,
    const char* const* optionTexts, int optionCount)
{
    if (g_coopRole != CoopRole::Host || g_coopConnState != CoopConnState::Connected) {
        return;
    }

    CoopDialogueState state;
    memset(&state, 0, sizeof(state));
    state.replyMessageListId = replyListId;
    state.replyMessageId = replyMsgId;
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
        state.optionMessageListId[i] = optionListIds[i];
        state.optionMessageId[i] = optionMsgIds[i];
        state.optionReaction[i] = optionReactions[i];
        strncpy(state.optionText[i], optionTexts[i] != NULL ? optionTexts[i] : "", sizeof(state.optionText[i]) - 1);
    }

    bool sent = coopnet_send_message(g_coopPeerSocket, COOP_MSG_DIALOGUE_STATE, &state, sizeof(state));
    debug_printf("\nCoop: notified peer dialogue state (reply=\"%.40s\" options=%d) success=%d\n", state.replyText, count, sent);
}

void coopnet_notify_dialogue_visual_begin(int headFid, int reaction)
{
    if (g_coopRole != CoopRole::Host || g_coopConnState != CoopConnState::Connected) {
        return;
    }

    CoopDialogueVisualBegin msg;
    msg.headFid = headFid;
    msg.reaction = reaction;
    msg.targetTile = dialog_target != NULL ? dialog_target->tile : -1;
    msg.targetElevation = dialog_target != NULL ? dialog_target->elevation : 0;
    msg.background = gdialog_get_background();

    bool sent = coopnet_send_message(g_coopPeerSocket, COOP_MSG_DIALOGUE_VISUAL_BEGIN, &msg, sizeof(msg));
    debug_printf("\nCoop: notified peer dialogue visual began (headFid=%d reaction=%d) success=%d\n", headFid, reaction, sent);
}

void coopnet_notify_head_frame(int fid, int frame)
{
    if (g_coopRole != CoopRole::Host || g_coopConnState != CoopConnState::Connected || fid == -1) {
        return;
    }

    CoopHeadFrame msg;
    msg.fid = fid;
    msg.frame = frame;
    coopnet_send_message(g_coopPeerSocket, COOP_MSG_HEAD_FRAME, &msg, sizeof(msg));
}

void coopnet_notify_dialogue_visual_end()
{
    if (g_coopRole != CoopRole::Host || g_coopConnState != CoopConnState::Connected) {
        return;
    }

    bool sent = coopnet_send_message(g_coopPeerSocket, COOP_MSG_DIALOGUE_VISUAL_END, NULL, 0);
    debug_printf("\nCoop: notified peer dialogue visual ended, success=%d\n", sent);
}

// Host-side only: last state actually sent, so coopnet_notify_worldmap_state()
// (called unconditionally every worldmap loop iteration -- see world_map()'s
// own comment) only puts a message on the wire when something a player would
// notice actually changed, same reasoning as coopnet_dialogue_sync_tick()'s
// diff against gdialog.cc's own snapshot. Reset to "definitely different"
// by coopnet_notify_worldmap_begin() so the very first state of a session
// always goes out.
static int g_coopLastSentWorldmapTerrain = -1;
static bool g_coopLastSentWorldmapIsMoving = false;

void coopnet_notify_worldmap_begin()
{
    g_coopLastSentWorldmapTerrain = -1;
    g_coopLastSentWorldmapIsMoving = false;

    // The streamed host screen (coopnet_travel_screen_begin) replaced the
    // client's old terrain-only mirror overlay.
    if (g_coopRole != CoopRole::Host || g_coopConnState != CoopConnState::Connected || g_coopRemoteHostActive) {
        return;
    }

    bool sent = coopnet_send_message(g_coopPeerSocket, COOP_MSG_WORLDMAP_BEGIN, NULL, 0);
    debug_printf("\nCoop: notified peer worldmap began, success=%d\n", sent);
}

void coopnet_notify_worldmap_end()
{
    if (g_coopRole != CoopRole::Host || g_coopConnState != CoopConnState::Connected || g_coopRemoteHostActive) {
        return;
    }

    bool sent = coopnet_send_message(g_coopPeerSocket, COOP_MSG_WORLDMAP_END, NULL, 0);
    debug_printf("\nCoop: notified peer worldmap ended, success=%d\n", sent);
}

void coopnet_notify_worldmap_state(int terrain, bool isMoving)
{
    if (g_coopRole != CoopRole::Host || g_coopConnState != CoopConnState::Connected || g_coopRemoteHostActive) {
        return;
    }

    if (terrain == g_coopLastSentWorldmapTerrain && isMoving == g_coopLastSentWorldmapIsMoving) {
        return;
    }
    g_coopLastSentWorldmapTerrain = terrain;
    g_coopLastSentWorldmapIsMoving = isMoving;

    CoopWorldmapState state;
    state.terrain = static_cast<uint8_t>(terrain);
    state.isMoving = isMoving ? 1 : 0;

    bool sent = coopnet_send_message(g_coopPeerSocket, COOP_MSG_WORLDMAP_STATE, &state, sizeof(state));
    debug_printf("\nCoop: notified peer worldmap state (terrain=%d moving=%d) success=%d\n", terrain, isMoving, sent);
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

// ---------------------------------------------------------------------------
// The F9 co-op screen
// ---------------------------------------------------------------------------

struct CoopMenuLine {
    char text[200];
    int color;
};

static void coopnet_menu_push_line(std::vector<CoopMenuLine>& out, const char* text, int color)
{
    CoopMenuLine line;
    snprintf(line.text, sizeof(line.text), "%s", text);
    line.color = color;
    out.push_back(line);
}

// Word-wraps `text` to `maxWidth` pixels (current font), keeping leading
// spaces as an indent on every wrapped line.
static void coopnet_menu_wrap(const char* text, int maxWidth, int color, std::vector<CoopMenuLine>& out)
{
    if (text[0] == '\0') {
        coopnet_menu_push_line(out, "", color);
        return;
    }

    char indent[16] = "";
    int indentLen = 0;
    while (text[indentLen] == ' ' && indentLen < 12) {
        indent[indentLen] = ' ';
        indentLen++;
    }
    indent[indentLen] = '\0';

    char line[200];
    snprintf(line, sizeof(line), "%s", indent);
    bool lineHasWord = false;
    char word[100];
    int wordLen = 0;

    for (const char* p = text + indentLen;; p++) {
        char c = *p;
        if (c == ' ' || c == '\0') {
            word[wordLen] = '\0';
            if (wordLen > 0) {
                char candidate[300];
                if (lineHasWord) {
                    snprintf(candidate, sizeof(candidate), "%s %s", line, word);
                } else {
                    snprintf(candidate, sizeof(candidate), "%s%s", line, word);
                }

                if (lineHasWord && text_width(candidate) > maxWidth) {
                    coopnet_menu_push_line(out, line, color);
                    snprintf(line, sizeof(line), "%s%s", indent, word);
                } else {
                    snprintf(line, sizeof(line), "%s", candidate);
                }
                lineHasWord = true;
            }
            wordLen = 0;
            if (c == '\0') {
                break;
            }
        } else if (wordLen < static_cast<int>(sizeof(word)) - 1) {
            word[wordLen++] = c;
        }
    }

    coopnet_menu_push_line(out, line, color);
}

static int coopnet_menu_severity_color(int severity)
{
    switch (severity) {
    case COOP_STATUS_GOOD:
        return colorTable[992]; // green
    case COOP_STATUS_WARN:
        return colorTable[32736]; // yellow
    case COOP_STATUS_BAD:
        return colorTable[31744]; // red
    default:
        return colorTable[32767]; // white
    }
}

static void coopnet_menu_format_time(uint32_t ms, char* out, size_t outSize)
{
    uint32_t seconds = ms / 1000;
    snprintf(out, outSize, "[%u:%02u]", seconds / 60, seconds % 60);
}

// coop_report.txt: everything a bug report needs, next to the exe.
static bool coopnet_write_report()
{
    FILE* f = fopen("coop_report.txt", "w");
    if (f == NULL) {
        return false;
    }

    const char* roleName = g_coopRole == CoopRole::Host ? "HOST" : (g_coopRole == CoopRole::Client ? "CLIENT" : "none");
    fprintf(f, "Fallout Companion Coop - report\n");
    fprintf(f, "Build: %s %s, protocol %u\n", __DATE__, __TIME__, kCoopProtocolVersion);
    fprintf(f, "Role: %s   connection state: %d   connected: %s\n", roleName, static_cast<int>(g_coopConnState), coopnet_is_connected() ? "yes" : "no");
    fprintf(f, "Launch label: \"%s\"   join target: %s:%d (attempts: %d)\n", g_coopInstanceLabel, g_coopClientIp, g_coopClientPort, g_coopClientAttempts);
    fprintf(f, "Host: connection attempts received: %d\n", g_coopHostIncomingAttempts);
    fprintf(f, "Map: %s   in combat: %s\n", map_data.name, isInCombat() ? "yes" : "no");

    char ips[8][64];
    int ipCount = coopnet_collect_local_ips(ips, 8);
    fprintf(f, "\nThis PC's addresses:\n");
    for (int i = 0; i < ipCount; i++) {
        fprintf(f, "   %s\n", ips[i]);
    }

    fprintf(f, "\n== Connection log ==\n");
    int statusCount = g_coopStatusTotal < kCoopStatusMaxLines ? g_coopStatusTotal : kCoopStatusMaxLines;
    for (int i = g_coopStatusTotal - statusCount; i < g_coopStatusTotal; i++) {
        const CoopStatusLine& line = g_coopStatusLines[i % kCoopStatusMaxLines];
        char stamp[24];
        coopnet_menu_format_time(line.ms, stamp, sizeof(stamp));
        fprintf(f, "%s %s\n", stamp, line.text);
    }

    fprintf(f, "\n== Glitches (%d recorded) ==\n", g_coopGlitchTotal);
    int glitchCount = g_coopGlitchTotal < kCoopGlitchMaxLines ? g_coopGlitchTotal : kCoopGlitchMaxLines;
    for (int i = g_coopGlitchTotal - glitchCount; i < g_coopGlitchTotal; i++) {
        const CoopGlitchLine& line = g_coopGlitchLines[i % kCoopGlitchMaxLines];
        char stamp[24];
        coopnet_menu_format_time(line.ms, stamp, sizeof(stamp));
        if (line.repeat > 1) {
            fprintf(f, "%s %s  (x%d)\n", stamp, line.text, line.repeat);
        } else {
            fprintf(f, "%s %s\n", stamp, line.text);
        }
    }

    fclose(f);
    return true;
}

static void coopnet_menu_load_last_ip(char* out, size_t outSize)
{
    FILE* f = fopen("coop_last_ip.txt", "r");
    if (f == NULL) {
        return;
    }
    char buffer[64] = "";
    if (fgets(buffer, sizeof(buffer), f) != NULL) {
        size_t len = strlen(buffer);
        while (len > 0 && (buffer[len - 1] == '\n' || buffer[len - 1] == '\r' || buffer[len - 1] == ' ')) {
            buffer[--len] = '\0';
        }
        snprintf(out, outSize, "%s", buffer);
    }
    fclose(f);
}

static void coopnet_menu_save_last_ip(const char* ip)
{
    FILE* f = fopen("coop_last_ip.txt", "w");
    if (f != NULL) {
        fprintf(f, "%s\n", ip);
        fclose(f);
    }
}

enum CoopMenuMode {
    COOP_MENU_PICK = 0,
    COOP_MENU_HOST = 1,
    COOP_MENU_JOIN = 2,
};

enum CoopMenuKey {
    COOP_MENU_KEY_LEFT = 4001,
    COOP_MENU_KEY_MIDDLE = 4002,
    COOP_MENU_KEY_TAB_CONNECTION = 4003,
    COOP_MENU_KEY_TAB_GLITCHES = 4004,
};

void coopnet_open_menu()
{
    if (g_coopMenuOpen) {
        return;
    }

    coopnet_sockets_init();

    const int W = 580;
    const int H = 420;
    const int pad = 14;

    int mode = COOP_MENU_PICK;
    if (g_coopRole == CoopRole::Host) {
        mode = COOP_MENU_HOST;
    } else if (g_coopRole == CoopRole::Client || g_coopClientRetrying) {
        mode = COOP_MENU_JOIN;
    } else if (strcmp(g_coopInstanceLabel, "HOST") == 0) {
        mode = COOP_MENU_HOST;
    } else if (strcmp(g_coopInstanceLabel, "CLIENT") == 0) {
        mode = COOP_MENU_JOIN;
    }

    // The address being typed (digits and dots only, which also keeps every
    // letter free for the menu's own shortcuts).
    char ip[64] = "";
    if (g_coopClientIp[0] != '\0') {
        snprintf(ip, sizeof(ip), "%s", g_coopClientIp);
    } else if (g_coopConnectTargetGiven) {
        snprintf(ip, sizeof(ip), "%s", g_coopConnectTarget);
    } else {
        coopnet_menu_load_last_ip(ip, sizeof(ip));
    }

    bool launchedAsJoin = strcmp(g_coopInstanceLabel, "CLIENT") == 0;
    if (mode == COOP_MENU_HOST && g_coopRole == CoopRole::None) {
        coopnet_start_host(kCoopDefaultPort);
    } else if (mode == COOP_MENU_JOIN && launchedAsJoin && g_coopConnectTargetGiven
        && g_coopRole == CoopRole::None && !g_coopClientRetrying) {
        // Launched with an explicit --coop-connect address (the test setup):
        // F9 connects straight away, as it always did.
        coopnet_menu_save_last_ip(ip);
        coopnet_start_client(ip, kCoopDefaultPort);
    }

    char hostIps[6][64];
    int hostIpCount = coopnet_collect_local_ips(hostIps, 6);

    bool bkWasEnabled = map_disable_bk_processes();
    cycle_disable();
    bool mouseWasVisible = gmouse_3d_is_on();
    if (mouseWasVisible) {
        gmouse_3d_off();
    }
    gmouse_set_cursor(MOUSE_CURSOR_ARROW);

    int x = (screenGetWidth() - W) / 2;
    int y = (screenGetHeight() - H) / 2 - 20;
    if (y < 0) {
        y = 0;
    }

    int win = win_add(x, y, W, H, 256, WINDOW_MODAL | WINDOW_DONT_MOVE_TOP);
    if (win == -1) {
        coopnet_status(COOP_STATUS_BAD, "Couldn't open the co-op screen (window creation failed).");
        if (mouseWasVisible) {
            gmouse_3d_on();
        }
        if (bkWasEnabled) {
            map_enable_bk_processes();
        }
        cycle_enable();
        return;
    }

    int oldFont = text_curr();
    text_font(101);
    const int lineHeight = text_height() + 2;

    const int footerY = H - 40;
    const int footerH = 28;
    const int footerGap = 8;
    const int footerW = (W - 2 * pad - 2 * footerGap) / 3;
    const int tabsY = pad + lineHeight + 4;
    const int bodyTop = tabsY + lineHeight + 10;
    const int bodyBottom = footerY - 10;
    const int textWidthMax = W - 2 * pad;

    win_register_button(win, pad, footerY, footerW, footerH, -1, -1, -1, COOP_MENU_KEY_LEFT, NULL, NULL, NULL, BUTTON_FLAG_TRANSPARENT);
    win_register_button(win, pad + footerW + footerGap, footerY, footerW, footerH, -1, -1, -1, COOP_MENU_KEY_MIDDLE, NULL, NULL, NULL, BUTTON_FLAG_TRANSPARENT);
    win_register_button(win, pad + 2 * (footerW + footerGap), footerY, footerW, footerH, -1, -1, -1, KEY_ESCAPE, NULL, NULL, NULL, BUTTON_FLAG_TRANSPARENT);
    win_register_button(win, pad, tabsY - 2, 190, lineHeight + 4, -1, -1, -1, COOP_MENU_KEY_TAB_CONNECTION, NULL, NULL, NULL, BUTTON_FLAG_TRANSPARENT);
    win_register_button(win, pad + 200, tabsY - 2, 220, lineHeight + 4, -1, -1, -1, COOP_MENU_KEY_TAB_GLITCHES, NULL, NULL, NULL, BUTTON_FLAG_TRANSPARENT);

    g_coopMenuOpen = true;

    int page = 0; // 0 = connection, 1 = glitches
    bool connectedAtOpen = coopnet_is_connected();
    bool done = false;
    bool needRedraw = true;
    bool reportSaved = false;
    uint32_t connectedSince = 0;
    uint32_t lastBlinkPhase = 0;
    int lastStatusTotal = -1;
    int lastGlitchTotal = -1;
    int lastState = -1;
    int lastMode = -1;
    bool lastActive = false;

    while (!done) {
        sharedFpsLimiter.mark();

        coopnet_poll();

        bool active = g_coopClientRetrying || g_coopRole != CoopRole::None;
        bool connected = coopnet_is_connected();
        if (!connected) {
            connectedAtOpen = false;
        }

        // The client's session carries on straight into the game: close the
        // menu the moment it becomes connected, before the host's map/world
        // sync is applied (doing that with this window open would be asking
        // for trouble). The host's menu lingers a moment so it shows the
        // news. Opening the menu while ALREADY connected just shows status.
        if (connected && !connectedAtOpen && mode == COOP_MENU_JOIN) {
            break;
        }
        if (connected && !connectedAtOpen && mode == COOP_MENU_HOST) {
            if (connectedSince == 0) {
                connectedSince = coopnet_now_ms();
            } else if (coopnet_now_ms() - connectedSince > 1500) {
                break;
            }
        } else {
            connectedSince = 0;
        }

        int key = get_input();
        if (key != -1 && key != -2) {
            int lower = key;
            if (lower >= 'A' && lower <= 'Z') {
                lower += 'a' - 'A';
            }

            bool joinIdle = mode == COOP_MENU_JOIN && !active;

            if (key == KEY_ESCAPE) {
                done = true;
            } else if (key == KEY_TAB) {
                page = 1 - page;
                needRedraw = true;
            } else if (key == COOP_MENU_KEY_TAB_CONNECTION) {
                page = 0;
                needRedraw = true;
            } else if (key == COOP_MENU_KEY_TAB_GLITCHES) {
                page = 1;
                needRedraw = true;
            } else if (lower == 'r' || (key == COOP_MENU_KEY_MIDDLE && mode != COOP_MENU_PICK)) {
                reportSaved = coopnet_write_report();
                if (reportSaved) {
                    coopnet_status(COOP_STATUS_GOOD, "Report saved to coop_report.txt (next to the game) - send it to the mod author.");
                } else {
                    coopnet_status(COOP_STATUS_BAD, "Couldn't write coop_report.txt (is the game folder read-only?).");
                }
                needRedraw = true;
            } else if (mode == COOP_MENU_PICK && (lower == 'h' || key == COOP_MENU_KEY_LEFT)) {
                mode = COOP_MENU_HOST;
                page = 0;
                if (g_coopRole == CoopRole::None) {
                    coopnet_start_host(kCoopDefaultPort);
                }
                needRedraw = true;
            } else if (mode == COOP_MENU_PICK && (lower == 'j' || key == COOP_MENU_KEY_MIDDLE)) {
                mode = COOP_MENU_JOIN;
                page = 0;
                needRedraw = true;
            } else if (mode == COOP_MENU_HOST && (lower == 's' || key == COOP_MENU_KEY_LEFT)) {
                coopnet_stop_session();
                coopnet_status(COOP_STATUS_INFO, "Stopped hosting.");
                mode = COOP_MENU_PICK;
                needRedraw = true;
            } else if (mode == COOP_MENU_JOIN && active && (lower == 's' || key == COOP_MENU_KEY_LEFT)) {
                coopnet_stop_session();
                coopnet_status(COOP_STATUS_INFO, "Cancelled.");
                needRedraw = true;
            } else if (joinIdle && (key == KEY_RETURN || key == COOP_MENU_KEY_LEFT)) {
                if (ip[0] == '\0') {
                    coopnet_status(COOP_STATUS_WARN, "Type the host's address first (the numbers your friend gives you).");
                } else {
                    coopnet_menu_save_last_ip(ip);
                    coopnet_start_client(ip, kCoopDefaultPort);
                }
                needRedraw = true;
            } else if (joinIdle && key == KEY_BACKSPACE) {
                size_t len = strlen(ip);
                if (len > 0) {
                    ip[len - 1] = '\0';
                }
                needRedraw = true;
            } else if (joinIdle && key == KEY_CTRL_V) {
                char* clip = SDL_GetClipboardText();
                if (clip != NULL) {
                    size_t len = strlen(ip);
                    for (const char* p = clip; *p != '\0' && len < 40; p++) {
                        if ((*p >= '0' && *p <= '9') || *p == '.') {
                            ip[len++] = *p;
                        }
                    }
                    ip[len] = '\0';
                    SDL_free(clip);
                }
                needRedraw = true;
            } else if (joinIdle && ((key >= '0' && key <= '9') || key == '.')) {
                size_t len = strlen(ip);
                if (len < 40) {
                    ip[len] = static_cast<char>(key);
                    ip[len + 1] = '\0';
                }
                needRedraw = true;
            }
        }

        if (game_user_wants_to_quit != 0) {
            done = true;
        }

        uint32_t blinkPhase = coopnet_now_ms() / 500;
        int state = static_cast<int>(g_coopConnState);
        if (g_coopStatusTotal != lastStatusTotal || g_coopGlitchTotal != lastGlitchTotal
            || state != lastState || mode != lastMode || active != lastActive
            || (mode == COOP_MENU_JOIN && !active && blinkPhase != lastBlinkPhase)) {
            needRedraw = true;
        }

        if (needRedraw) {
            needRedraw = false;
            lastStatusTotal = g_coopStatusTotal;
            lastGlitchTotal = g_coopGlitchTotal;
            lastState = state;
            lastMode = mode;
            lastActive = active;
            lastBlinkPhase = blinkPhase;

            unsigned char* buf = win_get_buf(win);
            win_fill(win, 0, 0, W, H, colorTable[0]);
            win_border(win);

            const int colGreen = colorTable[992];
            const int colWhite = colorTable[32767];
            const int colDim = colorTable[14798];
            const int colYellow = colorTable[32736];

            // Title + one-line role summary.
            char title[160];
            if (mode == COOP_MENU_HOST) {
                snprintf(title, sizeof(title), "CO-OP   -   HOSTING");
            } else if (mode == COOP_MENU_JOIN) {
                snprintf(title, sizeof(title), "CO-OP   -   JOINING A FRIEND");
            } else {
                snprintf(title, sizeof(title), "CO-OP");
            }
            text_to_buf(buf + W * pad + pad, title, W - pad, W, colWhite);

            // Tabs.
            char tab0[64] = "[ CONNECTION ]";
            char tab1[64];
            snprintf(tab1, sizeof(tab1), "[ GLITCHES (%d) ]", g_coopGlitchTotal);
            text_to_buf(buf + W * tabsY + pad, tab0, W - pad, W, page == 0 ? colGreen : colDim);
            text_to_buf(buf + W * tabsY + pad + 200, tab1, W - pad - 200, W, page == 1 ? colGreen : colDim);
            win_line(win, pad, tabsY + lineHeight + 3, W - pad, tabsY + lineHeight + 3, colDim);

            std::vector<CoopMenuLine> header;
            std::vector<CoopMenuLine> log;

            if (page == 0) {
                if (mode == COOP_MENU_PICK) {
                    coopnet_menu_wrap("Play together over the network. One player HOSTS (loads a save and stays in the game), the other JOINS by typing the host's address.", textWidthMax, colWhite, header);
                    coopnet_menu_push_line(header, "", colWhite);
                    coopnet_menu_push_line(header, "   H  -  host a game", colGreen);
                    coopnet_menu_push_line(header, "   J  -  join a friend's game", colGreen);
                } else if (mode == COOP_MENU_HOST) {
                    char line[160];
                    if (connected) {
                        coopnet_menu_push_line(header, "Status: your friend is CONNECTED", colGreen);
                    } else if (g_coopRole == CoopRole::Host) {
                        snprintf(line, sizeof(line), "Status: waiting for your friend (port %d)", kCoopDefaultPort);
                        coopnet_menu_push_line(header, line, colYellow);
                    } else {
                        coopnet_menu_push_line(header, "Status: not hosting", colDim);
                    }
                    snprintf(line, sizeof(line), "Connection attempts received so far: %d", g_coopHostIncomingAttempts);
                    coopnet_menu_push_line(header, line, colWhite);
                    if (hostIpCount > 0) {
                        coopnet_menu_push_line(header, "Give your friend one of these addresses:", colWhite);
                        for (int i = 0; i < hostIpCount; i++) {
                            snprintf(line, sizeof(line), "      %s", hostIps[i]);
                            coopnet_menu_push_line(header, line, colGreen);
                        }
                    }
                } else {
                    char line[160];
                    bool cursorOn = (blinkPhase % 2) == 0;
                    if (!active) {
                        snprintf(line, sizeof(line), "Host address:   %s%s", ip, cursorOn ? "_" : " ");
                        coopnet_menu_push_line(header, line, colGreen);
                        coopnet_menu_push_line(header, "Type the numbers your friend gave you (Ctrl+V pastes), then press Enter.", colDim);
                    } else if (connected) {
                        coopnet_menu_push_line(header, "Status: CONNECTED to the host", colGreen);
                    } else {
                        snprintf(line, sizeof(line), "Status: trying %s:%d  (attempt %d, keeps retrying for 2 minutes)", g_coopClientIp, g_coopClientPort, g_coopClientAttempts);
                        coopnet_menu_push_line(header, line, colYellow);
                    }
                }

                coopnet_menu_push_line(header, "", colWhite);
                coopnet_menu_push_line(header, "Log:", colDim);

                int statusCount = g_coopStatusTotal < kCoopStatusMaxLines ? g_coopStatusTotal : kCoopStatusMaxLines;
                for (int i = g_coopStatusTotal - statusCount; i < g_coopStatusTotal; i++) {
                    const CoopStatusLine& entry = g_coopStatusLines[i % kCoopStatusMaxLines];
                    char stamped[200];
                    char stamp[24];
                    coopnet_menu_format_time(entry.ms, stamp, sizeof(stamp));
                    snprintf(stamped, sizeof(stamped), "%s %s", stamp, entry.text);
                    coopnet_menu_wrap(stamped, textWidthMax, coopnet_menu_severity_color(entry.severity), log);
                }
            } else {
                char line[160];
                snprintf(line, sizeof(line), "Things the sync code noticed going wrong (%d so far). Press R to save a report to send to the mod author.", g_coopGlitchTotal);
                coopnet_menu_wrap(line, textWidthMax, colWhite, header);
                coopnet_menu_push_line(header, "", colWhite);

                int glitchCount = g_coopGlitchTotal < kCoopGlitchMaxLines ? g_coopGlitchTotal : kCoopGlitchMaxLines;
                if (glitchCount == 0) {
                    coopnet_menu_push_line(log, "Nothing recorded - no glitches so far.", colGreen);
                }
                for (int i = g_coopGlitchTotal - glitchCount; i < g_coopGlitchTotal; i++) {
                    const CoopGlitchLine& entry = g_coopGlitchLines[i % kCoopGlitchMaxLines];
                    char stamped[220];
                    char stamp[24];
                    coopnet_menu_format_time(entry.ms, stamp, sizeof(stamp));
                    if (entry.repeat > 1) {
                        snprintf(stamped, sizeof(stamped), "%s %s (x%d)", stamp, entry.text, entry.repeat);
                    } else {
                        snprintf(stamped, sizeof(stamped), "%s %s", stamp, entry.text);
                    }
                    coopnet_menu_wrap(stamped, textWidthMax, colYellow, log);
                }
            }

            // Draw the header, then as much of the newest log as still fits.
            int drawY = bodyTop;
            int maxLines = (bodyBottom - bodyTop) / lineHeight;
            int headerLines = static_cast<int>(header.size());
            for (int i = 0; i < headerLines && i < maxLines; i++) {
                text_to_buf(buf + W * drawY + pad, header[i].text, W - pad, W, header[i].color);
                drawY += lineHeight;
            }
            int remaining = maxLines - headerLines;
            int logCount = static_cast<int>(log.size());
            int firstLog = logCount > remaining ? logCount - remaining : 0;
            for (int i = firstLog; i < logCount && remaining > 0; i++) {
                text_to_buf(buf + W * drawY + pad, log[i].text, W - pad, W, log[i].color);
                drawY += lineHeight;
            }

            // Footer buttons.
            const char* leftLabel = "";
            const char* middleLabel = "";
            if (mode == COOP_MENU_PICK) {
                leftLabel = "HOST  (H)";
                middleLabel = "JOIN  (J)";
            } else if (mode == COOP_MENU_HOST) {
                leftLabel = "STOP HOSTING  (S)";
                middleLabel = "SAVE REPORT  (R)";
            } else if (active) {
                leftLabel = connected ? "DISCONNECT  (S)" : "CANCEL  (S)";
                middleLabel = "SAVE REPORT  (R)";
            } else {
                leftLabel = "CONNECT  (Enter)";
                middleLabel = "SAVE REPORT  (R)";
            }
            const char* labels[3] = { leftLabel, middleLabel, "CLOSE  (Esc)" };
            for (int i = 0; i < 3; i++) {
                int bx = pad + i * (footerW + footerGap);
                win_line(win, bx, footerY, bx + footerW, footerY, colDim);
                win_line(win, bx, footerY + footerH, bx + footerW, footerY + footerH, colDim);
                win_line(win, bx, footerY, bx, footerY + footerH, colDim);
                win_line(win, bx + footerW, footerY, bx + footerW, footerY + footerH, colDim);
                if (labels[i][0] != '\0') {
                    int labelWidth = text_width(labels[i]);
                    text_to_buf(buf + W * (footerY + (footerH - text_height()) / 2) + bx + (footerW - labelWidth) / 2, labels[i], footerW, W, colGreen);
                }
            }

            win_draw(win);
        }

        renderPresent();
        sharedFpsLimiter.throttle();
    }

    g_coopMenuOpen = false;
    win_delete(win);
    tile_refresh_display();
    text_font(oldFont);

    if (mouseWasVisible) {
        gmouse_3d_on();
    }
    if (bkWasEnabled) {
        map_enable_bk_processes();
    }
    cycle_enable();
    gmouse_set_cursor(MOUSE_CURSOR_ARROW);
}

// ---------------------------------------------------------------------------
// Joining from the MAIN MENU (F9 there): pick or create a character, type the
// host's address, go. No single-player world is involved -- the player never
// has to start a New Game or load a save just to be able to connect; the
// host's world is what gets loaded.
// ---------------------------------------------------------------------------

struct CoopCharEntry {
    char path[80];
    char name[40];
    int level;
};

static char g_coopPendingJoinIp[64] = "";

static void coopnet_chars_ensure_dir()
{
    char* patches = NULL;
    if (config_get_string(&game_config, GAME_CONFIG_SYSTEM_KEY, GAME_CONFIG_MASTER_PATCHES_KEY, &patches) && patches != NULL) {
        char dir[COMPAT_MAX_PATH];
        snprintf(dir, sizeof(dir), "%s\\%s", patches, "COOPCHARS");
        compat_mkdir(dir);
    }
}

static void coopnet_chars_path_for(const char* name, char* out, size_t outSize)
{
    char clean[40];
    int length = 0;
    for (const char* p = name; *p != '\0' && length < 30; p++) {
        unsigned char c = static_cast<unsigned char>(*p);
        if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
            clean[length++] = static_cast<char>(c);
        }
    }
    if (length == 0) {
        snprintf(clean, sizeof(clean), "CHARACTER");
    } else {
        clean[length] = '\0';
    }
    snprintf(out, outSize, "COOPCHARS\\%s.CHR", clean);
}

static void coopnet_chars_refresh(std::vector<CoopCharEntry>& out)
{
    out.clear();

    char** files = NULL;
    int count = db_get_file_list("COOPCHARS\\*.CHR", &files, NULL, 0);
    if (count > 0 && files != NULL) {
        for (int i = 0; i < count; i++) {
            CoopCharEntry entry;
            snprintf(entry.path, sizeof(entry.path), "COOPCHARS\\%s", files[i]);
            if (pc_coop_peek_data(entry.path, entry.name, sizeof(entry.name), &entry.level) == 0) {
                out.push_back(entry);
            }
        }
    }
    if (files != NULL) {
        db_free_file_list(&files, NULL);
    }
}

bool coopnet_main_menu_join()
{
    coopnet_chars_ensure_dir();

    std::vector<CoopCharEntry> chars;
    coopnet_chars_refresh(chars);

    int selected = 0;
    char ip[64] = "";
    if (g_coopClientIp[0] != '\0') {
        snprintf(ip, sizeof(ip), "%s", g_coopClientIp);
    } else {
        coopnet_menu_load_last_ip(ip, sizeof(ip));
    }

    bool cursorWasHidden = mouse_hidden();
    if (cursorWasHidden) {
        mouse_show();
    }
    gmouse_set_cursor(MOUSE_CURSOR_ARROW);

    loadColorTable("color.pal");
    palette_fade_to(cmap);

    const int W = 580;
    const int H = 420;
    const int pad = 14;
    const int maxRows = 7;

    int win = -1;
    int oldFont = text_curr();
    int lineHeight = 16;
    int footerY = H - 40;
    const int footerH = 28;
    const int footerGap = 8;
    const int footerW = (W - 2 * pad - 2 * footerGap) / 3;
    int listTop = 0;

    auto openWindow = [&]() -> bool {
        int x = (screenGetWidth() - W) / 2;
        int y = (screenGetHeight() - H) / 2;
        win = win_add(x, y, W, H, colorTable[0], WINDOW_MODAL | WINDOW_DONT_MOVE_TOP);
        if (win == -1) {
            return false;
        }
        text_font(101);
        lineHeight = text_height() + 2;
        listTop = pad + lineHeight * 3 + 6;

        win_register_button(win, pad, footerY, footerW, footerH, -1, -1, -1, KEY_RETURN, NULL, NULL, NULL, BUTTON_FLAG_TRANSPARENT);
        win_register_button(win, pad + footerW + footerGap, footerY, footerW, footerH, -1, -1, -1, 6002, NULL, NULL, NULL, BUTTON_FLAG_TRANSPARENT);
        win_register_button(win, pad + 2 * (footerW + footerGap), footerY, footerW, footerH, -1, -1, -1, KEY_ESCAPE, NULL, NULL, NULL, BUTTON_FLAG_TRANSPARENT);
        for (int row = 0; row < maxRows; row++) {
            win_register_button(win, pad, listTop + row * lineHeight, W - 2 * pad, lineHeight, -1, -1, -1, 6100 + row, NULL, NULL, NULL, BUTTON_FLAG_TRANSPARENT);
        }
        return true;
    };

    if (!openWindow()) {
        text_font(oldFont);
        if (cursorWasHidden) {
            mouse_hide();
        }
        return false;
    }

    char message[160] = "";
    int messageColor = 0;
    bool result = false;
    bool done = false;
    bool needRedraw = true;
    uint32_t lastBlink = 0;

    while (!done) {
        sharedFpsLimiter.mark();

        int key = get_input();
        if (key != -1 && key != -2) {
            int lower = key;
            if (lower >= 'A' && lower <= 'Z') {
                lower += 'a' - 'A';
            }

            if (key == KEY_ESCAPE) {
                done = true;
            } else if (key == KEY_ARROW_UP) {
                if (selected > 0) {
                    selected--;
                }
                needRedraw = true;
            } else if (key == KEY_ARROW_DOWN) {
                if (selected + 1 < static_cast<int>(chars.size()) && selected + 1 < maxRows) {
                    selected++;
                }
                needRedraw = true;
            } else if (key >= 6100 && key < 6100 + maxRows) {
                if (key - 6100 < static_cast<int>(chars.size())) {
                    selected = key - 6100;
                }
                needRedraw = true;
            } else if (lower == 'n' || key == 6002) {
                // The normal character selector / creation screens, exactly as
                // New Game uses them.
                win_delete(win);
                win = -1;
                text_font(oldFont);

                ResetPlayer();
                int rc = select_character();
                if (rc == 2) {
                    char path[80];
                    coopnet_chars_path_for(critter_name(obj_dude), path, sizeof(path));
                    if (pc_coop_save_data(path) == 0) {
                        snprintf(message, sizeof(message), "Saved character \"%s\".", critter_name(obj_dude));
                        messageColor = 1;
                    } else {
                        snprintf(message, sizeof(message), "Couldn't save the character file (%s).", path);
                        messageColor = 3;
                    }
                    coopnet_chars_refresh(chars);
                    for (size_t i = 0; i < chars.size(); i++) {
                        if (strcmp(chars[i].path, path) == 0) {
                            selected = static_cast<int>(i);
                        }
                    }
                } else {
                    message[0] = '\0';
                }

                loadColorTable("color.pal");
                palette_fade_to(cmap);
                if (!openWindow()) {
                    break;
                }
                needRedraw = true;
            } else if (key == KEY_RETURN) {
                if (chars.empty()) {
                    snprintf(message, sizeof(message), "Create a character first (press N).");
                    messageColor = 2;
                } else if (ip[0] == '\0') {
                    snprintf(message, sizeof(message), "Type the host's address first.");
                    messageColor = 2;
                } else if (pc_coop_load_data(chars[selected].path) != 0) {
                    snprintf(message, sizeof(message), "That character file couldn't be read.");
                    messageColor = 3;
                } else {
                    stat_recalc_derived(obj_dude);
                    proto_dude_update_gender();
                    critter_adjust_hits(obj_dude, 1000);

                    coopnet_menu_save_last_ip(ip);
                    snprintf(g_coopPendingJoinIp, sizeof(g_coopPendingJoinIp), "%s", ip);
                    snprintf(g_coopActiveCharPath, sizeof(g_coopActiveCharPath), "%s", chars[selected].path);
                    result = true;
                    done = true;
                }
                needRedraw = true;
            } else if (key == KEY_BACKSPACE) {
                size_t len = strlen(ip);
                if (len > 0) {
                    ip[len - 1] = '\0';
                }
                needRedraw = true;
            } else if (key == KEY_CTRL_V) {
                char* clip = SDL_GetClipboardText();
                if (clip != NULL) {
                    size_t len = strlen(ip);
                    for (const char* p = clip; *p != '\0' && len < 40; p++) {
                        if ((*p >= '0' && *p <= '9') || *p == '.') {
                            ip[len++] = *p;
                        }
                    }
                    ip[len] = '\0';
                    SDL_free(clip);
                }
                needRedraw = true;
            } else if ((key >= '0' && key <= '9') || key == '.') {
                size_t len = strlen(ip);
                if (len < 40) {
                    ip[len] = static_cast<char>(key);
                    ip[len + 1] = '\0';
                }
                needRedraw = true;
            }
        }

        if (game_user_wants_to_quit != 0) {
            done = true;
        }

        uint32_t blink = coopnet_now_ms() / 500;
        if (blink != lastBlink) {
            lastBlink = blink;
            needRedraw = true;
        }

        if (needRedraw && win != -1) {
            needRedraw = false;

            unsigned char* buf = win_get_buf(win);
            win_fill(win, 0, 0, W, H, colorTable[0]);
            win_border(win);

            const int colGreen = colorTable[992];
            const int colWhite = colorTable[32767];
            const int colDim = colorTable[14798];
            const int colYellow = colorTable[32736];
            const int colRed = colorTable[31744];

            text_to_buf(buf + W * pad + pad, "CO-OP   -   JOIN A FRIEND'S GAME", W - pad, W, colWhite);
            text_to_buf(buf + W * (pad + lineHeight) + pad, "Pick your character, or press N to create a new one:", W - pad, W, colDim);

            if (chars.empty()) {
                text_to_buf(buf + W * listTop + pad, "No co-op characters yet - press N to create one.", W - pad, W, colYellow);
            }
            for (int row = 0; row < static_cast<int>(chars.size()) && row < maxRows; row++) {
                char line[120];
                snprintf(line, sizeof(line), "%s %s   (level %d)", row == selected ? ">" : " ", chars[row].name, chars[row].level);
                text_to_buf(buf + W * (listTop + row * lineHeight) + pad, line, W - pad, W, row == selected ? colGreen : colDim);
            }

            int addressY = listTop + maxRows * lineHeight + 16;
            char addressLine[120];
            snprintf(addressLine, sizeof(addressLine), "Host address:   %s%s", ip, (lastBlink % 2) == 0 ? "_" : " ");
            text_to_buf(buf + W * addressY + pad, addressLine, W - pad, W, colGreen);
            text_to_buf(buf + W * (addressY + lineHeight) + pad, "Type the numbers your friend gave you (Ctrl+V pastes), then press Enter.", W - pad, W, colDim);

            if (message[0] != '\0') {
                int color = messageColor == 1 ? colGreen : (messageColor == 3 ? colRed : colYellow);
                text_to_buf(buf + W * (addressY + 3 * lineHeight) + pad, message, W - pad, W, color);
            }

            const char* labels[3] = { "CONNECT  (Enter)", "NEW CHARACTER  (N)", "BACK  (Esc)" };
            for (int i = 0; i < 3; i++) {
                int bx = pad + i * (footerW + footerGap);
                win_line(win, bx, footerY, bx + footerW, footerY, colDim);
                win_line(win, bx, footerY + footerH, bx + footerW, footerY + footerH, colDim);
                win_line(win, bx, footerY, bx, footerY + footerH, colDim);
                win_line(win, bx + footerW, footerY, bx + footerW, footerY + footerH, colDim);
                int labelWidth = text_width(labels[i]);
                text_to_buf(buf + W * (footerY + (footerH - text_height()) / 2) + bx + (footerW - labelWidth) / 2, labels[i], footerW, W, colGreen);
            }

            win_draw(win);
        }

        renderPresent();
        sharedFpsLimiter.throttle();
    }

    if (win != -1) {
        win_delete(win);
    }
    text_font(oldFont);

    palette_fade_to(black_palette);

    if (cursorWasHidden) {
        mouse_hide();
    }

    return result;
}

void coopnet_main_menu_join_begin()
{
    if (g_coopPendingJoinIp[0] == '\0') {
        return;
    }

    char ip[64];
    snprintf(ip, sizeof(ip), "%s", g_coopPendingJoinIp);
    g_coopPendingJoinIp[0] = '\0';

    coopnet_start_client(ip, kCoopDefaultPort);

    // Show the connection progress straight away; it closes by itself once
    // connected.
    coopnet_open_menu();
}

void coopnet_end_session()
{
    coopnet_stop_session();
}

} // namespace fallout
