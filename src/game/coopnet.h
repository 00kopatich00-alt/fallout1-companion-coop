#ifndef FALLOUT_GAME_COOPNET_H_
#define FALLOUT_GAME_COOPNET_H_

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

} // namespace fallout

#endif /* FALLOUT_GAME_COOPNET_H_ */
