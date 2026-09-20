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

} // namespace fallout

#endif /* FALLOUT_GAME_COOPNET_H_ */
