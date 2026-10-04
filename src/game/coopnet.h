#ifndef FALLOUT_GAME_COOPNET_H_
#define FALLOUT_GAME_COOPNET_H_

#include <cstdint>

#include "game/object_types.h"

namespace fallout {

enum class CoopRole {
    None,
    Host,
    Client,
};

// Default TCP port for the milestone-1 debug host/join entry point (F11).
const int kCoopDefaultPort = 29999;

// Scans command-line arguments for the coop debug flag ("--coop-debug"), which
// allows running multiple copies of the game in the same Windows session
// (bypassing the single-instance mutexes in winmain.cc/amutex.cc) for local
// same-machine testing. Must be called once, early, before any mutex creation.
void coopnet_parse_command_line(int argc, char** argv);

// True if the coop debug flag was present on the command line.
bool coopnet_allow_multiple_instances();

// An optional label parsed from "--coop-name=<label>" on the command line
// (e.g. "HOST"/"CLIENT"), used to distinguish multiple same-titled windows
// when testing on one machine. Empty string if not provided.
const char* coopnet_get_instance_label();

// The address to connect to when acting as a client, from "--coop-connect=<ip>"
// on the command line. Defaults to "127.0.0.1" (same-machine testing) if not
// provided.
const char* coopnet_get_connect_target();

// Starts hosting a LAN coop session on the given TCP port (non-blocking
// listen). Finds-or-spawns the dedicated companion object near obj_dude.
// Returns false if the socket couldn't be created/bound.
bool coopnet_start_host(int port);

// Connects to a host at the given IP/port as a client (non-blocking connect).
// Returns false if the socket couldn't be created or the address is invalid.
bool coopnet_start_client(const char* ip, int port);

// Tears down any active session (socket(s) closed, role reset to None).
void coopnet_shutdown();

// The F9 co-op screen: host or join (typed IP), a live connection status log
// for both sides (who connected, why an attempt failed), and a "Glitches"
// page listing everything the sync code has noticed going wrong, with a Save
// Report button that writes coop_report.txt next to the exe for bug reports.
// Blocks (its own input loop) until closed.
void coopnet_open_menu();

// Main-menu co-op entry (F9 on the main menu): pick one of your saved co-op
// characters or create a new one, type the host's address. Returns true when
// the player wants to go in; false to return to the main menu. Nothing about a
// single-player game is started by this.
bool coopnet_main_menu_join();

// Call right after the game world has been set up for that join (the same
// point New Game would start playing): starts connecting and shows progress.
void coopnet_main_menu_join_begin();

// Ends whatever session/retry the player started (back at the main menu).
void coopnet_end_session();

// Host: the host's party just earned experience (called from
// stat_pc_add_experience() with the raw amount). The client gets the same
// gain and levels up on its own PC. No-op unless hosting with a client.
void coopnet_host_notify_xp(int xp);

// Client: the character screen was just closed -- skill points, perks or traits
// may have changed, so tell the host and update the character file.
void coopnet_on_character_screen_closed();

// Called at the very start of game_reset() (every save load / new game): the
// companion has to be removed BEFORE the engine frees all prototypes, because
// it would otherwise survive as a critter with no prototype and crash the
// first time anything reads its stats.
void coopnet_on_game_reset();

// The display name of the client's character ("Companion" until the client's
// own character name is known).
const char* coopnet_get_companion_name();

// Records something that went wrong in the sync code (printf-style) on the
// co-op menu's Glitches page and in the report, as well as the debug log.
void coopnet_report_glitch(const char* fmt, ...);

// Must be called exactly once per main-loop tick (see main.cc). Drives the
// network state machine: accepts/connects, drains incoming messages and
// applies them, sends periodic position broadcasts (host) or the host-side
// fallback follow-the-leader behavior for the companion while no client is
// connected.
void coopnet_poll();

CoopRole coopnet_get_role();
bool coopnet_is_connected();

// The dedicated coop companion object, or NULL if none has been established
// yet (host: not yet spawned/found; client: not yet resolved from handshake).
Object* coopnet_get_companion();

// Called from gmouse.cc when the local role is Client: instead of moving the
// client's own (mirrored, non-authoritative) obj_dude, sends the clicked tile
// as a move-intent for the companion to the host.
void coopnet_on_client_click(int tile);

// Called from gmouse.cc when the local role is Client and the player clicks
// to pick up a ground item: instead of picking it up with the client's own
// (mirrored, non-authoritative) obj_dude -- which fights every frame with
// the position-sync code also driving that same object, and was the cause
// of obj_dude's visible appearance corrupting on pickup -- sends a pickup
// request identifying the item by (pid, tile, elevation) for the host to
// apply to the real, authoritative companion object.
void coopnet_on_client_pickup_click(int pid, int tile, int elevation);

// Called from gmouse.cc when the local role is Client and the player clicks
// to use a piece of scenery (e.g. a door): sends a use request identifying
// the target by (pid, tile, elevation) for the host to apply to the real,
// authoritative companion object, same reasoning as the pickup click above.
// NOTE: the resulting state change (e.g. a door opening) is only applied on
// the host's own authoritative world -- it is not yet broadcast back to
// update the client's independently-loaded copy of the same object. Known
// limitation, see coopnet_host_process_action_queue() in coopnet.cc.
void coopnet_on_client_use_click(int pid, int tile, int elevation);

// Called from gmouse.cc when the local role is Client and the player uses a
// skill (Lockpick, Steal, Traps, First Aid, Doctor, Science, Repair) on a
// target: sends a skill request identifying the target by (pid, tile,
// elevation), same reasoning as the pickup/use clicks above. Companion
// stats/skills need no separate sync -- the companion shares obj_dude's own
// pid (0x1000000), which proto_ptr() special-cases to a single shared
// pc_proto struct, so SPECIAL stats, skill points, and level/experience are
// already the same underlying data for both, not a copy.
//
// Takes the target Object* itself (not raw pid/tile/elevation) so it can
// special-case `target == obj_dude` -- see CoopSkillRequest's comment in
// coopnet.cc for why targeting the host's own character needs this.
void coopnet_on_client_skill_use(int skill, Object* target);

// Called from inventry.cc's handle_inventory() when the client opens the
// companion's inventory during the companion's own combat turn: tells the
// host to deduct the usual vanilla AP cost (4 - Quick Pockets perk level,
// same formula handle_inventory() already applies to obj_dude's own case)
// from the real, authoritative companion object. The host recomputes the
// cost itself rather than trusting a value from here, and the resulting AP
// change reaches the client automatically via the existing turn-AP
// broadcast (coopnet_combat_input()'s own diff-and-resend loop in
// coopnet.cc) -- no separate reply is needed.
void coopnet_on_client_open_companion_inventory();

// Called from protinst.cc's obj_drop()/obj_pickup() right after they
// succeed, for whichever object performed the action. No-ops unless
// connected and `critter` is one of the two synced characters (obj_dude or
// the companion) — mirrors the change to the peer's world so both players
// see the same ground items. Identifies items by (pid, tile, elevation)
// rather than a shared unique id, so two identical items dropped on the
// same tile could be ambiguous — a known limitation, not expected to matter
// in normal play.
void coopnet_notify_item_dropped(Object* critter, Object* item);
void coopnet_notify_item_picked_up(Object* critter, Object* item);

// Host-side only, called from scripts.cc in place of the normal
// inven_steal_container() modal loot UI when `stealer` is the coop
// companion -- there is no human at the host's keyboard meant to drive that
// UI for an action the client's player asked for. Auto-resolves the steal
// attempt (random item from `target`'s inventory, real skill_check_stealing()
// roll, no UI) instead. Returns true if an item was actually stolen.
bool coopnet_auto_resolve_companion_steal(Object* stealer, Object* target);

// Called from combat.cc's combat_turn() (host-side only) when it's the
// companion's turn in a synced combat and a client is connected: blocks
// until the companion's turn is over (AP exhausted, client sends end-turn,
// or a disconnect/timeout), driven by network messages instead of local AI
// or local player input. Movement only for now -- see coopnet.cc for the
// milestone-3 phasing.
void coopnet_combat_input(Object* companion);

// Client-side only: true while it's the companion's combat turn, per the
// most recent COOP_MSG_COMBAT_TURN received. Used to gate gmouse.cc's click
// handling and an end-turn key press between meaning something and being a
// no-op.
bool coopnet_is_companion_turn_active();

// Client-side only: true while a synced combat is happening on the host at
// all (per the most recent COOP_MSG_COMBAT_BEGIN/_END), regardless of
// whose turn it currently is. Unlike the engine's own isInCombat(), which
// reflects this process's own LOCAL simulation (NPCs run independently per
// side, so it can be true or false completely independent of the real,
// host-authoritative fight -- see combat()'s own role-guard comment in
// combat.cc for a concrete case this caused), this is the real, synced
// state. Needed anywhere a client-side check would otherwise reach for
// isInCombat() and get an unreliable answer -- e.g. handle_inventory()'s
// "is a fight happening that I need to wait my turn for" gate.
bool coopnet_is_client_in_synced_combat();

// Called from gmouse.cc when the local role is Client, we're in a synced
// combat, and it's the companion's turn: sends a move action instead of the
// exploration-mode move-intent coopnet_on_client_click() sends. Ignored by
// the host outside of the companion's actual turn window.
void coopnet_on_client_combat_move_click(int tile);

// Called when the client wants to end the companion's combat turn early
// (mirrors obj_dude's own KEY_RETURN behavior in combat_input()). No-op if
// it isn't currently the companion's turn.
void coopnet_on_client_end_turn();

// Called when the client wants the companion to attack during its own
// combat turn. Pass the specific enemy the player clicked on (a local
// object that mirrors one of the host's combat participants, see
// coopnet_apply_combat_participant() in coopnet.cc) to target it
// specifically, or NULL to let the host's combat_ai() auto-target instead
// (e.g. for the plain "A to attack" key with no click). Silently falls back
// to auto-target host-side if `target` isn't a currently-tracked
// participant (not a synced enemy at all, or it died before this round-
// tripped). No-op if it isn't currently the companion's turn.
void coopnet_on_client_attack(Object* target);

// Client side: the companion inventory window was just closed -- pushes the
// resulting inventory + equipped items to the host (the authoritative copy) and
// refreshes the companion's look (worn armor / wielded weapon).
void coopnet_on_client_inventory_closed();

// Client clicked a corpse to loot it: asks the host to send the companion.
void coopnet_on_client_loot_click(Object* critter);

// Host: called when the COMPANION reaches a corpse/container to loot. Runs the
// real loot screen with the client driving it. Returns true if handled.
bool coopnet_host_run_companion_loot(Object* looter, Object* container);

// World map / town map screens (host-run modal loops): both players SEE the
// host's screen; the client operates it only if it led the trip (see
// coopnet_note_client_led_exit()), otherwise it just watches. Call begin right
// before world_map()/town_map() and end right after.
void coopnet_travel_screen_begin();
void coopnet_travel_screen_end();

// Elevator selection screen (elevator.cc's elevator_select()) -- same
// begin/end streaming treatment as the world map. Call right before/after
// each of its two call sites in scripts.cc. Automatically figures out whether
// the client's companion or the host's own character triggered it (see
// coopnet_note_companion_used_object()) and drives/watches accordingly.
void coopnet_elevator_screen_begin();
void coopnet_elevator_screen_end();

// Host: the client's companion just used an item/scenery object -- call right
// before the actual action_use_an_object() so a screen that opens as a
// result (currently just the elevator) knows to let the client drive it.
void coopnet_note_companion_used_object();

// Host: repositions the companion to obj_dude's current tile/elevation --
// call after any teleport that's supposed to carry "the whole party" (an
// elevator arriving), since unlike a normal walked exit the client has no way
// to get there under its own steam. No-op off-host / while solo.
void coopnet_host_move_companion_with_dude();

// Host: the companion (client's character) just stepped onto an exit grid at
// `tile`. True only if that is the exact tile the client clicked, i.e. a
// deliberate exit (never the companion merely following the host).
bool coopnet_host_companion_exit_allowed(int tile);

// Host: the exit the client just triggered leads to a screen the client drives.
void coopnet_note_client_led_exit();

// Host: a map load (and the placement of the party that follows it) is in
// progress -- the host must not read obj_dude's position for the client's
// MAP_TRANSITION until it is done (the load pumps the network loop, and the
// dude still stands on his tile from the PREVIOUS map at that point, so the
// client's character was respawned at a bogus tile: "spawns in a random place").
void coopnet_host_map_load_enter();
void coopnet_host_map_load_leave();

struct CoopMapLoadScope {
    CoopMapLoadScope() { coopnet_host_map_load_enter(); }
    ~CoopMapLoadScope() { coopnet_host_map_load_leave(); }
};

// Client: the companion is about to use `item` (on `target`, NULL = itself).
// Sends the request to the host, which does the real use, and returns true
// (the caller must then skip its local effect). False = not applicable.
bool coopnet_client_forward_item_use(Object* user, Object* item, Object* target);

// Client-character persistence (loadsave.cc): the client's character is the
// host's companion object, which is not part of the game save. On every save
// the host writes its inventory/equipment/HP to `path` (a COOP.DAT beside
// SAVE.DAT); on every load it is read back and applied when hosting starts.
void coopnet_host_save_profile(const char* path);

// Called after a saved game was loaded, in either role: every object was freed,
// so companion/mirror bookkeeping is reset and the sync is redone.
void coopnet_on_game_loaded();
void coopnet_host_load_profile(const char* path);

// True on a client while it loads a host-ordered map (see gameMouseRefreshImmediately()).
bool coopnet_client_map_loading();

// Host: attack sound mirroring. action_attack() brackets the registration of
// an attack's animations; every register_object_play_sfx() made for that
// attacker in between is forwarded so the client hears the punch / shot too.
// Host: an interface sound was just played on the host; while the client is
// shown that screen (remote screen) it plays it too.
void coopnet_notify_screen_sfx(const char* soundName);
void coopnet_begin_attack_sfx(Object* attacker);
void coopnet_end_attack_sfx();
void coopnet_notify_attack_sfx(Object* owner, const char* soundName, int delay);

// Host: the client's companion has just walked up to an NPC it was sent to
// talk to; the conversation that starts next is driven by the client.
void coopnet_note_companion_reached_npc();

// Host: the host's own character (not the companion) is the one whose Talk
// click is opening this conversation -- takes priority over the "whoever's
// physically closer" guess coopnet_mark_dialogue_client_initiated() makes for
// NPC-initiated greetings, since that guess can otherwise misfire mid-way
// through this very click.
void coopnet_mark_dialogue_host_initiated();

// Same for stealing (client drives the steal screen, host watches).
bool coopnet_host_run_companion_steal(Object* thief, Object* target);

// Called from combat.cc's combat_begin()/combat_over() (host-side only, the
// same real transition points vanilla itself uses) to let the client know a
// synced combat has started/ended. Before this, the client's screen had no
// idea combat was happening at all -- host and client's own NPCs run
// independently/unsynced (see the general "simulation divergence"
// limitation), so nothing else told the client. This does not yet sync
// *what* the fight consists of (participants, enemy HP, etc.) -- just that
// one is happening, which is enough for the client to understand why the
// screen isn't responding to clicks except during the companion's own turn.
void coopnet_notify_combat_begin();
void coopnet_notify_combat_end();

// Host-side only. Call around a combat_display(attack) call (combat.cc,
// via actions.cc's two call sites) whenever attack->attacker or
// attack->defender is the companion: everything combat_display() prints
// in between (miss/hit/damage amount/critical/death -- whatever it
// decides, verbatim, same wording as the host's own screen) gets mirrored
// to the client as real combat log text, instead of the generic "Companion
// takes damage!" placeholder that was the only feedback before. The
// actual capture happens via a single hook in display_print() (display.cc)
// that calls coopnet_capture_display_print() unconditionally -- cheap
// no-op outside this window, which is the overwhelming majority of the
// time.
void coopnet_begin_capture_combat_text();
void coopnet_end_capture_combat_text();
void coopnet_capture_display_print(const char* text);

// Host-side only. Call from action_attack()'s own top (actions.cc) with
// the attacker and the anim code it already computed -- mirrors the
// attacker's real swing/point/fire animation to the client whenever the
// attacker is the companion or obj_dude. See coopnet_notify_attack_anim()'s
// own comment (coopnet.cc) for why this call site can't miss the way the
// first combat-text attempt did.
// `defender` is only used to tell the client which way the attacker faces
// while it fires (it turns to the target as part of the attack; the client has
// to be facing that way BEFORE the animation starts, because changing facing
// in the middle of one restarts it -- visible as skipped frames).
void coopnet_notify_attack_anim(Object* attacker, int anim, Object* defender);

// Host-side only. Call from show_damage_to_object()'s own top (actions.cc)
// with its exact same parameters -- mirrors that one real damage-reaction
// animation (hit/knockback/death) to the client whenever the object being
// animated is the companion or obj_dude. See coopnet_notify_damage_anim()'s
// own comment (coopnet.cc).
void coopnet_notify_damage_anim(Object* defender, int damage, int flags, bool hitFromFront, int knockbackDistance, int knockbackRotation, int anim, Object* attacker, int delay);

// Host-side only. Same idea and wire shape as coopnet_notify_attack_anim(),
// for non-combat one-shot gestures (picking an item up, reaching to use a
// door/scenery object). Call with the object and anim code right where
// a_use_obj()/action_get_an_object() (actions.cc) compute them, before
// their own register_object_animate() call. No-op unless obj is the
// companion or obj_dude.
void coopnet_notify_object_anim(Object* obj, int anim);

// Host-side only. Call from obj_use_door()'s own top (protinst.cc) with
// the door object and the state (open/closed) it's about to become.
// Mirrors it to the client so its independently-loaded copy of the same
// door object actually opens/closes too, instead of only playing the
// gesture animation while the door itself stays put. See
// coopnet_notify_scenery_state()'s own comment (coopnet.cc).
void coopnet_notify_scenery_state(Object* scenery, bool isOpen);

// Host-side only. Call from register_object_move_to_tile()/
// register_object_run_to_tile() (anim.cc) once the move is accepted, so
// the client can run the whole path in one animation instead of chasing
// position snapshots. No-op unless obj is the companion or obj_dude.
// `actionPoints` is the step limit the move was registered with (-1 = none).
void coopnet_notify_move(Object* obj, int tile, int elevation, bool run, int actionPoints);

// Same for register_object_move_to_object()/register_object_run_to_object():
// a move toward another object, which ends adjacent to it. Before this existed
// an enemy closing in on its target was never mirrored at all.
void coopnet_notify_move_to_object(Object* obj, Object* destination, bool run, int actionPoints);

// Client-side only. Call when a pipboy rest finishes (pipboy.cc's
// TimedRest()): sends the client's new game time to the host.
void coopnet_on_client_rest_finished();

// Client-side only. Asks the host to start a fight (the client never runs
// combat() itself), optionally against a clicked target (NULL = the A key,
// no specific target). A no-op if the client already knows about a synced
// fight. See coopnet_host_apply_combat_start() in coopnet.cc.
void coopnet_on_client_start_combat(Object* target);

// Client-side only. Call at the top of register_object_move/run_to_tile/
// _to_object (anim.cc): true means "this local move of the companion did not
// come from the network, refuse it". See coopnet.cc for the full story.
bool coopnet_block_local_move(Object* owner);

// True on a connected client: local writes to global variables (quest, story,
// karma state) are ignored -- the host's values, delivered by
// COOP_MSG_GVAR_DELTA, are the only authoritative ones.
bool coopnet_client_ignores_local_gvar_writes();

// True on a connected client: its own critter/spatial/combat script procs are
// skipped (the host simulates, world sync streams the result).
bool coopnet_client_freezes_local_scripts();

// The client's companion for camera/cursor/roof/HUD purposes: NULL while a
// host-ordered map load is in progress (see coopnet.cc), so callers fall back
// to obj_dude instead of dereferencing a half torn-down object.
Object* coopnet_get_view_companion();

// Host-side only, called from gdialog.cc when the host starts/ends a
// conversation with an NPC, and whenever the currently-displayed reply
// text/option list changes during one. The client can never initiate or
// affect dialogue itself -- see gmouse.cc's Client-role guard on the talk
// click -- this exists purely so the client can watch, read-only, what the
// host is doing (echoed into the client's own message log, or into the
// plain-text mirror window if the real visual system below never
// activates for this particular conversation -- see
// coopnet_notify_dialogue_visual_begin()'s comment for why both exist).
//
// replyListId/replyMsgId and each option's listId/msgId/reaction are the
// same (messageListId, messageId, reaction) triples gdialog.cc's own
// dialogBlock carries -- sent alongside the already-resolved text so the
// client's visual puppet (when active) can re-run the exact same local
// message-list lookup the host just did, which is also what triggers
// voice audio (see coopnet_notify_dialogue_visual_begin()'s comment). A
// listId of -4 (gdAddOptionStr()'s own sentinel for "literal text, no
// message-list entry") means there's nothing to look up -- the client
// should just use the text as sent, same as the host does.
void coopnet_notify_dialogue_begin();
void coopnet_notify_dialogue_end();

// Client-driven dialogue ("whoever starts it drives it"; see coopnet.cc).
// Client: the player clicked "talk" on an NPC / a key event while a
// conversation is running. Host: is the current (or about to start)
// conversation driven by the client, and the filter gDialogProcess() applies
// to its own input while the client drives.
void coopnet_on_client_talk_click(Object* target);
void coopnet_mark_dialogue_client_initiated(Object* npc);
bool coopnet_on_client_dialogue_key(int keyCode);
bool coopnet_dialogue_driven_by_client();
int coopnet_dialogue_filter_input(int keyCode);

// Client presses a command button on the puppet dialogue window: 2 = barter,
// 3 = "tell me about" (sent to the host as a driver command).
void coopnet_on_client_dialogue_command(int command);

// Remote screen: for a screen only the host can run but the client drives
// (barter, "tell me about"). The host wraps the screen's own loop with
// begin/end (no-ops unless the conversation is client-driven); the client is
// shown the host's screen and its mouse/keys are forwarded. renderPresent()
// (svga.cc) calls the frame hook every frame.
void coopnet_remote_begin();
void coopnet_remote_end();
bool coopnet_client_remote_active();
void coopnet_remote_screen_frame_hook();
void coopnet_after_client_barter();

// Floating text bubbles (NPC barks / script float_msg / combat taunts): the
// host forwards every one from text_object_create() (textobj.cc); a connected
// client refuses any it didn't receive from the host, so its own independent
// scripts can't make NPCs talk at unrelated moments.
void coopnet_notify_float_text(Object* obj, const char* text, int font, int color, int a5);
bool coopnet_client_blocks_local_float_text();
void coopnet_notify_dialogue_state(int replyListId, int replyMsgId, const char* replyText,
    const int* optionListIds, const int* optionMsgIds, const int* optionReactions,
    const char* const* optionTexts, int optionCount);

// Host-side only, called from gdialog.cc's scr_dialogue_init()/scr_dialogue_exit()
// -- NOT the same moment as coopnet_notify_dialogue_begin()/_end() above,
// which fire from gdialog_enter()/gdialog_exit() (the *outer* "a
// conversation is happening at all" boundary). scr_dialogue_init() only
// runs partway into that, once the NPC's own script actually calls the
// "start_gdialog" opcode to bring up the real graphical dialogue screen
// (background/frame art, animated head portrait, voice audio) -- the
// small minority of conversations whose script skips that opcode (no
// portrait, e.g. some barter-only or system NPCs) never call this at
// all, and stay on the plain-text mirror instead. headFid/reaction are
// scr_dialogue_init()'s own two parameters, passed through unchanged --
// the client's coopnet_client_begin_dialogue_visual() (gdialog.h) feeds
// them straight back into a real, local scr_dialogue_init() call, which
// is what actually builds the background/frame/portrait windows and
// starts the idle "fidget" animation (a registered background process,
// ticks on its own from then on -- no further per-frame driving needed
// from coop code). Voice audio needs no separate sync message at all:
// it's triggered as a side effect of the same (messageListId, messageId)
// lookup coopnet_notify_dialogue_state() already carries, reading the
// exact same local .msg file entry's audio filename the host's own
// lookup just used.
void coopnet_notify_dialogue_visual_begin(int headFid, int reaction);
void coopnet_notify_dialogue_visual_end();
// Host: a talking-head frame was just drawn (gdialog.cc); the client draws the same.
void coopnet_notify_head_frame(int fid, int frame);

// A cutscene is playing (gmovie.cc). Its own loop polls the network, and a map
// change handled from inside it -- the host noticing a new map, the client
// loading one -- ran in the middle of whatever was processing the game-time
// event that started the movie, and both games hung. While a movie plays the
// host does not announce map changes and the client holds them back; the held
// one is applied by coopnet_movie_end().
void coopnet_movie_begin();
void coopnet_movie_end();

// Host-side only, called from worldmap.cc when the host opens/closes the
// world map screen, and periodically (every loop iteration -- internally
// diffed against the last state actually sent, see
// coopnet_notify_worldmap_state()'s comment in coopnet.cc) while it's open.
// Worldmap travel is host-only by design (see scripts_request_worldmap()'s
// comment) -- the client can never open this screen itself, so without this
// its screen would just sit frozen with no feedback for however long the
// host spends traveling. `terrain` is a TerrainType (worldmap.h); coarse on
// purpose, not the host's exact coordinates -- see CoopWorldmapState's
// comment in coopnet.cc for why.
void coopnet_notify_worldmap_begin();
void coopnet_notify_worldmap_end();
void coopnet_notify_worldmap_state(int terrain, bool isMoving);

enum CoopGameOverReason : uint8_t {
    COOP_GAME_OVER_HOST_DIED = 0,
    COOP_GAME_OVER_COMPANION_DIED = 1,
};

// Host-side only, called from main.cc's own death check (when obj_dude --
// the host's own character -- dies) and internally within coopnet.cc (when
// the companion dies -- a shared game over by design: the companion dying
// ends the session for both players, not just the host, same as the host's
// own death would). Tells the client the shared game has ended and why,
// before the host's own process quits to the main menu -- without this the
// client's connection would just go silent with no explanation.
void coopnet_notify_game_over(uint8_t reason);

} // namespace fallout

#endif /* FALLOUT_GAME_COOPNET_H_ */
