#ifndef FALLOUT_GAME_COOP_AUTOTEST_H_
#define FALLOUT_GAME_COOP_AUTOTEST_H_

namespace fallout {

// Coop: a scripted self-test mode, so a game can be driven (and watched) without a person.
//
//   fallout-ce --autotest=script.txt [--autotest-out=folder] [--autotest-timeout=ms]
//
// The game presses keys and moves/clicks the mouse itself, by feeding the engine's own input
// queues (the same ones a real keyboard and mouse use), so it works in menus, dialogs and the
// map alike, with or without window focus. It writes screenshots of its own screen and a
// result.txt (PASS/FAIL per check) into the output folder and exits when the script says so.
// Nothing here depends on the operating system: it only uses SDL, the engine and the C library,
// so the same scripts run on Windows, Linux, macOS, Android and iOS builds.
//
// See docs/AUTOTEST.md for the script commands.

// Looks for --autotest=... in the command line. Does nothing when absent.
void autotest_parse_command_line(int argc, char** argv);

// True when a script was given and is still running.
bool autotest_active();

// True when --autotest-hidden was given: the game window is created hidden, so the test runs
// in the background (screenshots come from the game's own picture, not from the screen).
bool autotest_hidden();

// Body part a scripted aimed shot hits (0 head, 1 left arm, 2 right arm, 3 torso, 4 right leg, 5 left leg,
// 6 eyes, 7 groin), or -1 when the player picks it in the called-shot window.
int autotest_called_shot();

// The mouse buttons (1 left, 2 right) and keys the script is holding down right now -- input it feeds into
// the engine, which SDL's own hardware state does not show.
int autotest_mouse_buttons();
bool autotest_key_held(int scancode);

// True when --autotest-pos=x,y was given: where to put the game window (two games side by side for a recording).
bool autotest_window_position(int* x, int* y);

// Called from the engine's background-process hook (every time the game polls its input).
void autotest_tick();

// Called with every line the engine writes to its debug log; scripts wait for / check text in it.
void autotest_note_debug(const char* text);

} // namespace fallout

#endif /* FALLOUT_GAME_COOP_AUTOTEST_H_ */
