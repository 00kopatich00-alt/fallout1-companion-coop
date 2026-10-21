# Scripted self-tests (autotest)

A game started with `--autotest=<script.txt>` drives itself: it presses keys and moves/clicks the mouse by
feeding the engine's own input queues, so it works in menus, dialogs and on the map, with or without window
focus and without anybody touching the computer. It saves screenshots of its own screen and writes
`result.txt` (PASS / FAIL per check, and a SUMMARY line), then exits.

```
fallout-ce --autotest=script.txt [--autotest-out=folder] [--autotest-timeout=ms] [--coop-debug] [--coop-name=HOST|CLIENT]
```

`--coop-debug` lets two games run side by side on one PC. A game that has not finished after the timeout
(default 10 minutes) records a FAIL and exits.

## Running it on Windows

`tools/autotest/run_autotest.ps1` starts one game (`-HostScript`) or a host and a client (`-HostScript`,
`-ClientScript`) from the two test folders, waits for both, prints both result files and converts the
screenshots to PNG (`autotest_results/<name>/host|client`).

```
.\run_autotest.ps1 -HostScript .\autotest\pair_connect_host.txt -ClientScript .\autotest\pair_connect_client.txt
```

## Script commands (one per line, `#` starts a comment)

| Command | What it does |
|---|---|
| `wait <ms>` | Do nothing for a while. |
| `key <name> [holdMs]` | Press and release a key: a-z, 0-9, enter, esc, space, tab, backspace, up/down/left/right, f1-f12, home, end, pageup, pagedown, period, minus, shift, ctrl, alt. |
| `keys <name> <count>` | The same key several times (arrow down in a list). |
| `type <text>` | Type letters, digits, dots, dashes and spaces. |
| `mouse <x> <y>` | Move the pointer (screen pixels, 640x480 game coordinates). |
| `click <x> <y> [right] [double]` | Move, press, release. |
| `drag <x1> <y1> <x2> <y2>` | Press at the first point, move, release at the second. |
| `shot <name>` | Save `<name>.bmp` of the game's screen. |
| `log <text>` | Write a note into result.txt and the debug log. |
| `wait_for <ms> <text>` | Wait until the debug log contains the text (also counts what the last key or click caused). PASS, or FAIL on timeout. |
| `expect <text>` / `expect_not <text>` | The debug log has / has not got the text. |
| `on_fail continue\|abort` | What a FAIL does (default continue). A FAIL always saves `fail_N.bmp`. |
| `coop_host [port]` / `coop_join <ip> [port]` | Start hosting / join, like F9 does. |
| `quit [code]` | Write the SUMMARY and exit (the exit code is 1 when anything failed). |

Useful things to wait for in the log: `Coop-stage: main menu reached`, `LOADSAVE: Total load data read`,
`host: friend connected`, `client: HELLO_ACK received`, `client: map ... loaded`, `Coop-status: ...`.
Pressing any key skips the intro movies; the Load dialog takes `key enter` for the selected slot (`keys down N`
moves it).

## What it is good for / not good for

Good: crashes, hangs, connection failures, the same flow every time (regression), screenshots to look at,
logs to compare. Not good: judging how smooth an animation feels, real internet lag, or anything that depends
on the screen picture being "right" without somebody (or an image comparison) looking at it.

## Other platforms

The test code itself (`src/game/coop_autotest.cc`) only uses SDL, the engine and the C library, so it builds
wherever the engine does. What still has to be done per platform:

- **macOS / Linux:** the runner script (a bash version of `run_autotest.ps1`). The co-op code has a POSIX socket
  path but it has never been run there; the crash handler and the startup log are Windows-only (stubs elsewhere).
- **Android / iOS:** run with the script and output folder inside the app's storage (the command line is not
  available: read the script path from an environment variable or a file next to the game data), and fetch
  `result.txt` and the screenshots with adb / the file sharing of the device. The co-op menu (F9, typing an
  address) needs a touch version, and a phone that goes to the background suspends its network connection.
