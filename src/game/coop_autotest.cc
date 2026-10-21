#include "game/coop_autotest.h"

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

#include <SDL.h>

#include "game/actions.h"
#include "game/anim.h"
#include "game/gdialog.h"
#include "game/worldmap.h"
#include "game/combat.h"
#include "game/combat_defs.h"
#include "game/coopnet.h"
#include "game/critter.h"
#include "game/display.h"
#include "game/gmouse.h"
#include "game/intface.h"
#include "game/gmovie.h"
#include "game/inventry.h"
#include "game/map.h"
#include "game/object.h"
#include "game/party.h"
#include "game/perk.h"
#include "game/proto.h"
#include "game/proto_types.h"
#include "game/queue.h"
#include "game/scripts.h"
#include "game/item.h"
#include "game/stat.h"
#include "game/tile.h"
#include "game/trait.h"
#include "platform_compat.h"
#include "plib/gnw/debug.h"
#include "plib/gnw/kb.h"
#include "plib/gnw/mouse.h"
#include "plib/gnw/svga.h"

namespace fallout {

namespace {

enum StepKind {
    STEP_KEY_DOWN,
    STEP_KEY_UP,
    STEP_MOUSE_MOVE,
    STEP_MOUSE_BUTTONS,
};

struct Step {
    uint32_t delayMs; // wait this long after the previous step before doing this one
    StepKind kind;
    int a;
    int b;
};

bool g_active = false;
bool g_hidden = false;
bool g_havePos = false;
int g_calledShot = -1;
int g_posX = 0;
int g_posY = 0;
bool g_started = false;
bool g_inTick = false;
// How many script commands are being executed right now (a command can start a screen with its own
// input loop -- a death scene, a conversation -- and that loop must still be able to run the rest of
// the script, otherwise the game waits for a key the script can never press).
int g_commandDepth = 0;

std::vector<std::string> g_lines;
size_t g_next = 0;
std::string g_scriptPath;
std::string g_outDir = "autotest_out";
uint32_t g_timeoutMs = 600000;
uint32_t g_startMs = 0;

uint32_t g_waitUntil = 0;
std::deque<Step> g_steps;
uint32_t g_stepDue = 0;
int g_buttons = 0;
bool g_abortOnFail = false;
bool g_godMode = false;

// Where the log stood when the script last pressed or clicked something: a wait_for after it also
// sees what the press itself caused (loading a save prints its lines before the next command runs).
size_t g_actionMark = 0;
bool g_haveActionMark = false;

// wait_for <ms> <text>
bool g_waitingFor = false;
std::string g_waitText;
size_t g_waitMark = 0;
uint32_t g_waitDeadline = 0;

// The tail of everything the engine wrote to its debug log while the script runs.
std::string g_log;
size_t g_logDropped = 0;

int g_pass = 0;
int g_fail = 0;
int g_failShots = 0;
FILE* g_result = NULL;

size_t log_position()
{
    return g_logDropped + g_log.size();
}

bool log_contains_since(const std::string& text, size_t absoluteMark)
{
    size_t from = absoluteMark > g_logDropped ? absoluteMark - g_logDropped : 0;
    if (from > g_log.size()) {
        return false;
    }
    return g_log.find(text, from) != std::string::npos;
}

void result_line(const char* format, ...)
{
    char text[600];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);

    if (g_result != NULL) {
        fprintf(g_result, "%s\n", text);
        fflush(g_result);
    }
    debug_printf("\nAutotest: %s\n", text);
}

bool save_screenshot(const std::string& name)
{
    if (gSdlSurface == NULL) {
        return false;
    }
    std::string path = g_outDir + "/" + name + ".bmp";
    return SDL_SaveBMP(gSdlSurface, path.c_str()) == 0;
}

void check_failed(const std::string& what)
{
    g_fail++;
    result_line("FAIL %s", what.c_str());
    char name[40];
    snprintf(name, sizeof(name), "fail_%d", ++g_failShots);
    save_screenshot(name);
}

void check_passed(const std::string& what)
{
    g_pass++;
    result_line("PASS %s", what.c_str());
}

[[noreturn]] void finish(int code)
{
    result_line("SUMMARY pass=%d fail=%d exit=%d seconds=%u", g_pass, g_fail, code, (SDL_GetTicks() - g_startMs) / 1000);
    if (g_result != NULL) {
        fclose(g_result);
        g_result = NULL;
    }
    // Leave at once: the engine's own shutdown needs a player and a window.
    std::_Exit(code);
}

int scancode_for(const std::string& name)
{
    if (name.size() == 1) {
        char c = name[0];
        if (c >= 'a' && c <= 'z') return SDL_SCANCODE_A + (c - 'a');
        if (c >= 'A' && c <= 'Z') return SDL_SCANCODE_A + (c - 'A');
        if (c >= '1' && c <= '9') return SDL_SCANCODE_1 + (c - '1');
        if (c == '0') return SDL_SCANCODE_0;
        if (c == '.') return SDL_SCANCODE_PERIOD;
        if (c == '-') return SDL_SCANCODE_MINUS;
        if (c == ' ') return SDL_SCANCODE_SPACE;
        if (c == '=') return SDL_SCANCODE_EQUALS;
        if (c == ',') return SDL_SCANCODE_COMMA;
        return -1;
    }
    struct Named {
        const char* name;
        int code;
    };
    static const Named named[] = {
        { "enter", SDL_SCANCODE_RETURN }, { "return", SDL_SCANCODE_RETURN },
        { "esc", SDL_SCANCODE_ESCAPE }, { "escape", SDL_SCANCODE_ESCAPE },
        { "space", SDL_SCANCODE_SPACE }, { "tab", SDL_SCANCODE_TAB },
        { "backspace", SDL_SCANCODE_BACKSPACE }, { "delete", SDL_SCANCODE_DELETE },
        { "up", SDL_SCANCODE_UP }, { "down", SDL_SCANCODE_DOWN },
        { "left", SDL_SCANCODE_LEFT }, { "right", SDL_SCANCODE_RIGHT },
        { "home", SDL_SCANCODE_HOME }, { "end", SDL_SCANCODE_END },
        { "pageup", SDL_SCANCODE_PAGEUP }, { "pagedown", SDL_SCANCODE_PAGEDOWN },
        { "period", SDL_SCANCODE_PERIOD }, { "dot", SDL_SCANCODE_PERIOD },
        { "minus", SDL_SCANCODE_MINUS }, { "plus", SDL_SCANCODE_EQUALS },
        { "shift", SDL_SCANCODE_LSHIFT }, { "ctrl", SDL_SCANCODE_LCTRL }, { "alt", SDL_SCANCODE_LALT },
        { "f1", SDL_SCANCODE_F1 }, { "f2", SDL_SCANCODE_F2 }, { "f3", SDL_SCANCODE_F3 },
        { "f4", SDL_SCANCODE_F4 }, { "f5", SDL_SCANCODE_F5 }, { "f6", SDL_SCANCODE_F6 },
        { "f7", SDL_SCANCODE_F7 }, { "f8", SDL_SCANCODE_F8 }, { "f9", SDL_SCANCODE_F9 },
        { "f10", SDL_SCANCODE_F10 }, { "f11", SDL_SCANCODE_F11 }, { "f12", SDL_SCANCODE_F12 },
    };
    for (size_t i = 0; i < sizeof(named) / sizeof(named[0]); i++) {
        if (name == named[i].name) {
            return named[i].code;
        }
    }
    return -1;
}

void queue_key(int scancode, uint32_t holdMs, uint32_t delayBefore = 0)
{
    g_actionMark = log_position();
    g_haveActionMark = true;
    g_steps.push_back({ delayBefore, STEP_KEY_DOWN, scancode, 0 });
    g_steps.push_back({ holdMs, STEP_KEY_UP, scancode, 0 });
}

void queue_click(int x, int y, int button, bool twice)
{
    g_actionMark = log_position();
    g_haveActionMark = true;
    g_steps.push_back({ 0, STEP_MOUSE_MOVE, x, y });
    for (int i = 0; i < (twice ? 2 : 1); i++) {
        g_steps.push_back({ i == 0 ? 250u : 120u, STEP_MOUSE_BUTTONS, button, 0 });
        g_steps.push_back({ 180, STEP_MOUSE_BUTTONS, 0, 0 });
    }
}

void apply_step(const Step& step)
{
    switch (step.kind) {
    case STEP_KEY_DOWN:
    case STEP_KEY_UP: {
        KeyboardData data;
        data.key = step.a;
        data.down = step.kind == STEP_KEY_DOWN ? 1 : 0;
        kb_simulate_key(&data);
        break;
    }
    case STEP_MOUSE_MOVE: {
        int x, y;
        mouse_get_position(&x, &y);
        mouse_simulate_input(step.a - x, step.b - y, g_buttons);
        break;
    }
    case STEP_MOUSE_BUTTONS:
        g_buttons = step.a;
        mouse_simulate_input(0, 0, g_buttons);
        break;
    }
}

std::vector<std::string> split_words(const std::string& line)
{
    std::vector<std::string> words;
    size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) i++;
        size_t start = i;
        while (i < line.size() && line[i] != ' ' && line[i] != '\t') i++;
        if (i > start) {
            words.push_back(line.substr(start, i - start));
        }
    }
    return words;
}

// The text after the first `skip` words of the line.
std::string rest_of_line(const std::string& line, size_t skip)
{
    size_t i = 0;
    for (size_t w = 0; w < skip; w++) {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) i++;
        while (i < line.size() && line[i] != ' ' && line[i] != '\t') i++;
    }
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) i++;
    return line.substr(i);
}

int perk_for_name(const std::string& name)
{
    struct Named {
        const char* name;
        int perk;
    };
    static const Named names[] = {
        { "bonus_hth_attacks", PERK_BONUS_HTH_ATTACKS }, { "bonus_hth_damage", PERK_BONUS_HTH_DAMAGE },
        { "bonus_move", PERK_BONUS_MOVE }, { "bonus_ranged_damage", PERK_BONUS_RANGED_DAMAGE },
        { "bonus_rate_of_fire", PERK_BONUS_RATE_OF_FIRE }, { "more_criticals", PERK_MORE_CRITICALS },
        { "toughness", PERK_TOUGHNESS }, { "sharpshooter", PERK_SHARPSHOOTER },
        { "better_criticals", PERK_BETTER_CRITICALS }, { "slayer", PERK_SLAYER },
        { "sniper", PERK_SNIPER }, { "action_boy", PERK_ACTION_BOY },
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (name == names[i].name) {
            return names[i].perk;
        }
    }
    return atoi(name.c_str()) > 0 ? atoi(name.c_str()) : -1;
}

int trait_for_name(const std::string& name)
{
    struct Named {
        const char* name;
        int trait;
    };
    static const Named names[] = {
        { "fast_metabolism", TRAIT_FAST_METABOLISM }, { "bruiser", TRAIT_BRUISER }, { "small_frame", TRAIT_SMALL_FRAME },
        { "one_hander", TRAIT_ONE_HANDER }, { "finesse", TRAIT_FINESSE }, { "kamikaze", TRAIT_KAMIKAZE },
        { "heavy_handed", TRAIT_HEAVY_HANDED }, { "fast_shot", TRAIT_FAST_SHOT }, { "bloody_mess", TRAIT_BLOODY_MESS },
        { "jinxed", TRAIT_JINXED }, { "good_natured", TRAIT_GOOD_NATURED }, { "chem_reliant", TRAIT_CHEM_RELIANT },
        { "chem_resistant", TRAIT_CHEM_RESISTANT }, { "night_person", TRAIT_NIGHT_PERSON }, { "skilled", TRAIT_SKILLED },
        { "gifted", TRAIT_GIFTED },
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (name == names[i].name) {
            return names[i].trait;
        }
    }
    return -1;
}

// ---- explore <seconds> [seed]: a bot that plays on its own (travel the world map, fight what it meets, leave
// every map by an exit grid) so a long run shows crashes, hangs and desyncs in encounters and special places.
uint32_t g_exploreUntil = 0;
uint32_t g_exploreNext = 0;
uint32_t g_exploreNote = 0;
uint32_t g_exploreRng = 12345;
int g_exploreDecisions = 0;
int g_exploreCombatRounds = 0;
int g_exploreCombats = 0;
int g_exploreShots = 0;
int g_exploreDialogKeys = 0;
int g_exploreIdleRounds = 0;
int g_exploreLastTile = -1;
bool g_exploreWasCombat = false;
char g_exploreLastMap[20] = "";
std::vector<std::string> g_exploreMaps;
// Lines the bot wants run next, ahead of the script's own.
std::deque<std::string> g_injected;

int explore_random(int limit)
{
    g_exploreRng = g_exploreRng * 1664525u + 1013904223u;
    return static_cast<int>((g_exploreRng >> 8) % static_cast<uint32_t>(limit));
}

Object* explore_find_enemy()
{
    Object* best = NULL;
    int bestDistance = 1 << 30;
    for (Object* o = obj_find_first_at(map_elevation); o != NULL; o = obj_find_next_at()) {
        if (FID_TYPE(o->fid) != OBJ_TYPE_CRITTER || o == obj_dude || o == coopnet_get_companion() || o->tile == -1
            || (o->flags & OBJECT_HIDDEN) != 0 || critter_is_dead(o) || o->data.critter.combat.team == obj_dude->data.critter.combat.team) {
            continue;
        }
        Rect visible;
        obj_bound(o, &visible);
        int cx = (visible.ulx + visible.lrx) / 2;
        int cy = (visible.uly + visible.lry) / 2;
        if (cx < 20 || cx > 620 || cy < 20 || cy > 360) {
            continue;
        }
        int d = tile_dist(o->tile, obj_dude->tile);
        if (d < bestDistance) {
            bestDistance = d;
            best = o;
        }
    }
    return best;
}

Object* explore_nearest_exit(Object* me, bool anyOne)
{
    Object* best = NULL;
    int bestDistance = 1 << 30;
    int seen = 0;
    for (Object* o = obj_find_first_at(map_elevation); o != NULL; o = obj_find_next_at()) {
        if (FID_TYPE(o->fid) != OBJ_TYPE_MISC || o->pid < 0x5000010 || o->pid > 0x5000017 || o->tile == -1) {
            continue;
        }
        if (anyOne) {
            seen++;
            if (explore_random(seen) == 0) {
                best = o;
            }
            continue;
        }
        int d = tile_dist(o->tile, me->tile);
        if (d < bestDistance) {
            bestDistance = d;
            best = o;
        }
    }
    return best;
}

int explore_run_to(int tile)
{
    register_begin(ANIMATION_REQUEST_RESERVED);
    register_object_run_to_tile(obj_dude, tile, obj_dude->elevation, -1, 0);
    return register_end();
}

void explore_inject(const char* line)
{
    g_injected.push_back(line);
}

void explore_decide(uint32_t now)
{
    bool client = coopnet_get_role() == CoopRole::Client;
    g_godMode = true;
    g_exploreDecisions++;

    // A new map: note it, and keep a picture of the first ones.
    if (map_data.name[0] != '\0' && strcmp(map_data.name, g_exploreLastMap) != 0) {
        snprintf(g_exploreLastMap, sizeof(g_exploreLastMap), "%s", map_data.name);
        g_exploreMaps.push_back(g_exploreLastMap);
        result_line("EXPLORE map %s (map number %d, %d combats so far)", g_exploreLastMap, static_cast<int>(g_exploreMaps.size()), g_exploreCombats);
        if (g_exploreShots < 30) {
            char name[48];
            snprintf(name, sizeof(name), "explore_%02d_%s", ++g_exploreShots, g_exploreLastMap);
            save_screenshot(name);
        }
        g_exploreIdleRounds = 0;
    }
    if (now - g_exploreNote > 60000) {
        g_exploreNote = now;
        result_line("EXPLORE alive: map %s, combat %d, world map %d, %d decisions", g_exploreLastMap, isInCombat() ? 1 : 0, world_map_screen_open() ? 1 : 0, g_exploreDecisions);
    }

    bool combat = isInCombat() || (client && coopnet_is_client_in_synced_combat());
    if (combat && !g_exploreWasCombat) {
        g_exploreCombats++;
        g_exploreCombatRounds = 0;
        result_line("EXPLORE combat %d begins on %s", g_exploreCombats, g_exploreLastMap);
    }
    g_exploreWasCombat = combat;

    if (!client && world_map_screen_open()) {
        // Travel somewhere else on the map (the party dot's own surroundings enter a local map).
        char line[48];
        snprintf(line, sizeof(line), "click %d %d", 60 + explore_random(380), 60 + explore_random(360));
        explore_inject(line);
        explore_inject("wait 9000");
        return;
    }

    if (!client && dialog_active()) {
        // Easter eggs and strangers talk: answer, and leave if it drags on.
        if (++g_exploreDialogKeys > 24) {
            explore_inject("key esc");
        } else {
            const char* keys[] = { "key 1", "key 2", "key 1", "key 3" };
            explore_inject(keys[explore_random(4)]);
        }
        explore_inject("wait 1500");
        return;
    }
    g_exploreDialogKeys = 0;

    if (combat) {
        bool myTurn = client ? coopnet_is_companion_turn_active() : combat_whose_turn() == obj_dude;
        if (!myTurn) {
            return;
        }
        g_exploreCombatRounds++;
        if (g_exploreCombatRounds % 5 != 0 && explore_find_enemy() != NULL) {
            explore_inject("cursor attack");
            explore_inject("hover_enemy");
            explore_inject("wait 900");
            explore_inject("click_here");
            explore_inject("wait 5000");
        } else {
            explore_inject("key space");
            explore_inject("wait 3500");
        }
        return;
    }

    if (client) {
        return; // the client follows the host between fights
    }

    // Peaceful on a map: go to the nearest exit grid; if that goes nowhere, stroll somewhere else first.
    int anim = FID_ANIM_TYPE(obj_dude->fid);
    if (g_exploreDecisions < 16) {
        result_line("EXPLORE decision %d: my tile %d, anim %d, offset %d,%d, combat %d", g_exploreDecisions, obj_dude->tile, anim, obj_dude->x, obj_dude->y, isInCombat() ? 1 : 0);
    }
    if (anim == ANIM_WALK || anim == ANIM_RUNNING) {
        return;
    }
    if (obj_dude->tile == g_exploreLastTile) {
        g_exploreIdleRounds++;
    } else {
        g_exploreIdleRounds = 0;
    }
    g_exploreLastTile = obj_dude->tile;

    Object* exitObj = explore_nearest_exit(obj_dude, g_exploreIdleRounds >= 3);
    if (exitObj != NULL && g_exploreIdleRounds < 8) {
        int rc = explore_run_to(exitObj->tile);
        if (g_exploreDecisions % 10 == 0 || rc != 0) {
            result_line("EXPLORE run to exit tile %d, %d tiles away from tile %d: rc %d, idle rounds %d", exitObj->tile, tile_dist(exitObj->tile, obj_dude->tile), obj_dude->tile, rc, g_exploreIdleRounds);
        }
    } else {
        if (exitObj == NULL && g_exploreDecisions % 10 == 0) {
            result_line("EXPLORE no exit grid on %s", g_exploreLastMap);
        }
        int tile = obj_dude->tile + (explore_random(21) - 10) + 200 * (explore_random(21) - 10);
        if (tile >= 0 && tile < 40000) {
            explore_run_to(tile);
        }
        if (g_exploreIdleRounds > 10) {
            g_exploreIdleRounds = 0;
        }
    }
    explore_inject("wait 2500");
}

void explore_finish()
{
    std::string maps;
    for (size_t i = 0; i < g_exploreMaps.size(); i++) {
        maps += (i == 0 ? "" : ", ") + g_exploreMaps[i];
    }
    result_line("EXPLORE done: %d maps visited (%s), %d combats, %d decisions", static_cast<int>(g_exploreMaps.size()), maps.c_str(), g_exploreCombats, g_exploreDecisions);
    check_passed("explore finished");
}

void execute_line(const std::string& rawLine, uint32_t now)
{
    std::string line = rawLine;
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ' || line.back() == '\t')) {
        line.pop_back();
    }
    size_t firstChar = line.find_first_not_of(" \t");
    if (firstChar == std::string::npos || line[firstChar] == '#') {
        return;
    }

    std::vector<std::string> w = split_words(line);
    const std::string& cmd = w[0];

    if (cmd == "wait" && w.size() >= 2) {
        g_waitUntil = now + static_cast<uint32_t>(atoi(w[1].c_str()));
    } else if (cmd == "key" && w.size() >= 2) {
        int code = scancode_for(w[1]);
        if (code < 0) {
            check_failed("unknown key '" + w[1] + "'");
            return;
        }
        uint32_t hold = w.size() >= 3 ? static_cast<uint32_t>(atoi(w[2].c_str())) : 100u;
        queue_key(code, hold);
        g_waitUntil = now + 250; // the screen needs a moment to react
    } else if (cmd == "keys" && w.size() >= 3) {
        // keys <name> <count>: the same key several times (arrow down in a list)
        int code = scancode_for(w[1]);
        if (code < 0) {
            check_failed("unknown key '" + w[1] + "'");
            return;
        }
        for (int i = 0; i < atoi(w[2].c_str()); i++) {
            queue_key(code, 60, 140);
        }
    } else if (cmd == "type") {
        std::string text = rest_of_line(line, 1);
        for (size_t i = 0; i < text.size(); i++) {
            int code = scancode_for(std::string(1, text[i]));
            if (code >= 0) {
                queue_key(code, 50, 70);
            }
        }
    } else if (cmd == "mouse" && w.size() >= 3) {
        g_steps.push_back({ 0, STEP_MOUSE_MOVE, atoi(w[1].c_str()), atoi(w[2].c_str()) });
    } else if (cmd == "click" && w.size() >= 3) {
        int button = 0;
        bool twice = false;
        for (size_t i = 3; i < w.size(); i++) {
            if (w[i] == "right") button = MOUSE_STATE_RIGHT_BUTTON_DOWN;
            if (w[i] == "double") twice = true;
        }
        queue_click(atoi(w[1].c_str()), atoi(w[2].c_str()), button != 0 ? button : MOUSE_STATE_LEFT_BUTTON_DOWN, twice);
        g_waitUntil = now + 300;
    } else if (cmd == "drag" && w.size() >= 5) {
        g_steps.push_back({ 0, STEP_MOUSE_MOVE, atoi(w[1].c_str()), atoi(w[2].c_str()) });
        g_steps.push_back({ 100, STEP_MOUSE_BUTTONS, MOUSE_STATE_LEFT_BUTTON_DOWN, 0 });
        g_steps.push_back({ 150, STEP_MOUSE_MOVE, atoi(w[3].c_str()), atoi(w[4].c_str()) });
        g_steps.push_back({ 200, STEP_MOUSE_BUTTONS, 0, 0 });
    } else if (cmd == "shot" && w.size() >= 2) {
        if (save_screenshot(w[1])) {
            result_line("SHOT %s.bmp", w[1].c_str());
        } else {
            check_failed("screenshot " + w[1] + " could not be saved");
        }
    } else if (cmd == "log") {
        result_line("NOTE %s", rest_of_line(line, 1).c_str());
    } else if (cmd == "wait_for" && w.size() >= 3) {
        g_waitingFor = true;
        g_waitText = rest_of_line(line, 2);
        g_waitMark = g_haveActionMark ? g_actionMark : log_position();
        g_waitDeadline = now + static_cast<uint32_t>(atoi(w[1].c_str()));
    } else if (cmd == "expect") {
        std::string text = rest_of_line(line, 1);
        if (log_contains_since(text, 0)) {
            check_passed("expect '" + text + "'");
        } else {
            check_failed("expect '" + text + "' (not in the log)");
        }
    } else if (cmd == "expect_not") {
        std::string text = rest_of_line(line, 1);
        if (!log_contains_since(text, 0)) {
            check_passed("expect_not '" + text + "'");
        } else {
            check_failed("expect_not '" + text + "' (found in the log)");
        }
    } else if (cmd == "on_fail" && w.size() >= 2) {
        g_abortOnFail = w[1] == "abort";
    } else if (cmd == "coop_host") {
        bool ok = coopnet_start_host(w.size() >= 2 ? atoi(w[1].c_str()) : 29999);
        if (ok) {
            check_passed("coop_host");
        } else {
            check_failed("coop_host could not start hosting");
        }
    } else if (cmd == "coop_join" && w.size() >= 2) {
        bool ok = coopnet_start_client(w[1].c_str(), w.size() >= 3 ? atoi(w[2].c_str()) : 29999);
        if (ok) {
            check_passed("coop_join " + w[1]);
        } else {
            check_failed("coop_join " + w[1] + " could not start");
        }
    } else if (cmd == "perk" && w.size() >= 2) {
        // Test helper: give the player character a perk rank (no requirements).
        int perk = perk_for_name(w[1]);
        int count = w.size() >= 3 ? atoi(w[2].c_str()) : 1;
        int given = 0;
        for (int i = 0; i < count && perk >= 0; i++) {
            if (perk_add_forced(perk) == 0) {
                given++;
            }
        }
        if (perk >= 0 && given > 0) {
            check_passed("perk " + w[1] + " x" + std::to_string(given));
        } else {
            check_failed("perk " + w[1] + " could not be given");
        }
    } else if (cmd == "trait" && w.size() >= 2) {
        int a = trait_for_name(w[1]);
        int b = w.size() >= 3 ? trait_for_name(w[2]) : -1;
        if (a >= 0) {
            trait_set(a, b);
            check_passed("trait " + w[1] + (w.size() >= 3 ? " " + w[2] : ""));
        } else {
            check_failed("unknown trait " + w[1]);
        }
    } else if (cmd == "char_changed") {
        // Same thing that closing the character screen does: tell the host about the character.
        coopnet_on_character_screen_closed();
        check_passed("char_changed");
    } else if (cmd == "dump_companion" || cmd == "dump_dude") {
        Object* who = cmd == "dump_dude" ? obj_dude : coopnet_get_companion();
        if (who == NULL) {
            check_failed(cmd + ": no such character");
        } else {
            char perks[400] = "";
            for (int i = 0; i < PERK_COUNT; i++) {
                int rank = perk_level_of(who, i);
                if (rank > 0) {
                    char one[24];
                    snprintf(one, sizeof(one), " %d=%d", i, rank);
                    strncat(perks, one, sizeof(perks) - strlen(perks) - 1);
                }
            }
            result_line("DUMP %s hp=%d/%d ap=%d ac=%d crit=%d better_crit=%d seq=%d melee=%d dt=%d dr=%d perks:%s",
                cmd == "dump_dude" ? "dude" : "companion",
                who->data.critter.hp, stat_level(who, STAT_MAXIMUM_HIT_POINTS), stat_level(who, STAT_MAXIMUM_ACTION_POINTS),
                stat_level(who, STAT_ARMOR_CLASS), stat_level(who, STAT_CRITICAL_CHANCE), stat_level(who, STAT_BETTER_CRITICALS),
                stat_level(who, STAT_SEQUENCE), stat_level(who, STAT_MELEE_DAMAGE),
                stat_level(who, STAT_DAMAGE_THRESHOLD), stat_level(who, STAT_DAMAGE_RESISTANCE), perks);
        }
    } else if (cmd == "spawn" && w.size() >= 2) {
        // spawn <critter pid> [count] [distance]: hostile critters near the player (host side).
        int pid = atoi(w[1].c_str());
        int count = w.size() >= 3 ? atoi(w[2].c_str()) : 1;
        int distance = w.size() >= 4 ? atoi(w[3].c_str()) : 6;
        int made = 0;
        Object* shooterB = coopnet_get_companion();
        // Open ground with a clear line of fire from both players (a shot behind a wall is refused).
        for (int ring = distance; ring < distance + 6 && made < count; ring++) {
            for (int dir = 0; dir < 6 && made < count; dir++) {
                int tile = tile_num_in_direction(obj_dude->tile, dir, ring);
                if (tile == -1 || obj_blocking_at(NULL, tile, obj_dude->elevation) != NULL) {
                    continue;
                }
                if (combat_is_shot_blocked(obj_dude, obj_dude->tile, tile, NULL, NULL)
                    || (shooterB != NULL && combat_is_shot_blocked(shooterB, shooterB->tile, tile, NULL, NULL))) {
                    continue;
                }
                Object* critter = NULL;
                if (obj_pid_new(&critter, pid) == -1 || critter == NULL) {
                    continue;
                }
                Rect rect;
                obj_move_to_tile(critter, tile, obj_dude->elevation, &rect);
                critter->data.critter.combat.team = 1000;
                made++;
            }
        }
        tile_refresh_display();
        if (made > 0) {
            check_passed("spawn " + std::to_string(made) + " critter(s) of pid " + w[1]);
        } else {
            check_failed("spawn pid " + w[1] + " found no free tile");
        }
    } else if (cmd == "start_combat") {
        scripts_request_combat(NULL);
        check_passed("start_combat");
    } else if (cmd == "click_enemy" || cmd == "hover_enemy") {
        bool hoverOnly = cmd == "hover_enemy";
        // Click the nearest living critter that is not one of the players.
        Object* best = NULL;
        int bestDistance = 1 << 30;
        for (Object* o = obj_find_first_at(map_elevation); o != NULL; o = obj_find_next_at()) {
            if (FID_TYPE(o->fid) != OBJ_TYPE_CRITTER || o == obj_dude || o == coopnet_get_companion() || o->tile == -1
                || (o->flags & OBJECT_HIDDEN) != 0 || critter_is_dead(o) || o->data.critter.combat.team == obj_dude->data.critter.combat.team) {
                continue;
            }
            Rect visible;
            obj_bound(o, &visible);
            int cx = (visible.ulx + visible.lrx) / 2;
            int cy = (visible.uly + visible.lry) / 2;
            if (cx < 20 || cx > 620 || cy < 20 || cy > 360) {
                continue; // not on the screen
            }
            int d = tile_dist(o->tile, obj_dude->tile);
            if (d < bestDistance) {
                bestDistance = d;
                best = o;
            }
        }
        if (best == NULL) {
            check_failed("click_enemy: no enemy on the map");
        } else {
            Rect r;
            obj_bound(best, &r);
            int x = (r.ulx + r.lrx) / 2;
            int y = (r.uly + r.lry) / 2;
            result_line("NOTE click_enemy pid %d at tile %d, screen %d,%d", best->pid, best->tile, x, y);
            if (hoverOnly) {
                g_steps.push_back({ 0, STEP_MOUSE_MOVE, x, y });
                g_waitUntil = now + 900;
            } else {
                queue_click(x, y, MOUSE_STATE_LEFT_BUTTON_DOWN, false);
                g_waitUntil = now + 300;
            }
        }
    } else if (cmd == "explore" && w.size() >= 2) {
        g_exploreUntil = now + static_cast<uint32_t>(atoi(w[1].c_str())) * 1000u;
        g_exploreNext = now;
        g_exploreNote = now;
        if (w.size() >= 3) {
            g_exploreRng = static_cast<uint32_t>(atoi(w[2].c_str()));
        }
        result_line("EXPLORE starts for %s seconds", w[1].c_str());
    } else if (cmd == "goto_exit") {
        // Client: click the exit grid nearest to its own character (a combat move during a fight).
        Object* me = coopnet_get_view_companion();
        if (me == NULL) {
            me = obj_dude;
        }
        Object* best = NULL;
        int bestDistance = 1 << 30;
        for (Object* o = obj_find_first_at(map_elevation); o != NULL; o = obj_find_next_at()) {
            if (FID_TYPE(o->fid) != OBJ_TYPE_MISC || o->pid < 0x5000010 || o->pid > 0x5000017 || o->tile == -1) {
                continue;
            }
            int d = tile_dist(o->tile, me->tile);
            if (d < bestDistance) {
                bestDistance = d;
                best = o;
            }
        }
        if (best == NULL) {
            check_failed("goto_exit: no exit grid on this map level");
        } else {
            result_line("NOTE goto_exit tile %d, %d tiles away", best->tile, bestDistance);
            if (coopnet_is_client_in_synced_combat()) {
                coopnet_on_client_combat_move_click(best->tile);
            } else {
                coopnet_on_client_click(best->tile);
            }
            check_passed("goto_exit");
        }
    } else if (cmd == "xp" && w.size() >= 2) {
        stat_pc_add_experience(atoi(w[1].c_str()));
        check_passed("xp " + w[1]);
    } else if ((cmd == "hp" || cmd == "damage" || cmd == "rads" || cmd == "poison" || cmd == "give") && w.size() >= 3) {
        // <command> <dude|companion> <number> ...   (test helpers: set or change health, radiation, poison; give items)
        Object* who = w[1] == "dude" ? obj_dude : coopnet_get_companion();
        int amount = atoi(w[2].c_str());
        if (who == NULL) {
            check_failed(cmd + ": no " + w[1]);
        } else if (cmd == "hp") {
            who->data.critter.hp = amount;
            check_passed("hp " + w[1] + " " + w[2]);
        } else if (cmd == "damage") {
            critter_adjust_hits(who, -amount);
            check_passed("damage " + w[1] + " " + w[2]);
        } else if (cmd == "rads") {
            critter_adjust_rads(who, amount);
            check_passed("rads " + w[1] + " " + w[2]);
        } else if (cmd == "poison") {
            critter_adjust_poison(who, amount);
            check_passed("poison " + w[1] + " " + w[2]);
        } else {
            // give <who> <item pid> [quantity]
            Object* item = NULL;
            int quantity = w.size() >= 4 ? atoi(w[3].c_str()) : 1;
            if (obj_pid_new(&item, amount) == -1 || item == NULL || item_add_force(who, item, quantity) != 0) {
                check_failed("give " + w[1] + " pid " + w[2] + " failed");
            } else {
                obj_disconnect(item, NULL); // the inventory owns it now (else the next map load frees it from under the inventory)
                check_passed("give " + w[1] + " pid " + w[2] + " x" + std::to_string(quantity));
            }
        }
    } else if (cmd == "godmode" && w.size() >= 2) {
        g_godMode = w[1] == "on";
        check_passed("godmode " + w[1]);
    } else if (cmd == "start_fight") {
        // The nearest enemy attacks the player: that is how a fight starts in the real game.
        Object* attacker = NULL;
        int bestDistance = 1 << 30;
        for (Object* o = obj_find_first_at(map_elevation); o != NULL; o = obj_find_next_at()) {
            if (FID_TYPE(o->fid) != OBJ_TYPE_CRITTER || o == obj_dude || o == coopnet_get_companion() || o->tile == -1
                || critter_is_dead(o) || o->data.critter.combat.team == obj_dude->data.critter.combat.team) {
                continue;
            }
            int d = tile_dist(o->tile, obj_dude->tile);
            if (d < bestDistance) {
                bestDistance = d;
                attacker = o;
            }
        }
        if (attacker == NULL) {
            check_failed("start_fight: no enemy on the map");
        } else {
            STRUCT_664980 request;
            memset(&request, 0, sizeof(request));
            request.attacker = attacker;
            request.defender = obj_dude;
            request.minDamage = 0;
            request.maxDamage = 2147483647;
            scripts_request_combat(&request);
            check_passed("start_fight with pid " + std::to_string(attacker->pid));
        }
    } else if (cmd == "cursor" && w.size() >= 2) {
        gmouse_3d_set_mode(w[1] == "attack" ? GAME_MOUSE_MODE_CROSSHAIR : GAME_MOUSE_MODE_MOVE);
        check_passed("cursor " + w[1]);
    } else if (cmd == "wield" && w.size() >= 3) {
        // wield <dude|companion> <weapon pid> [ammo pid]: a loaded weapon in the right hand.
        Object* who = w[1] == "dude" ? obj_dude : coopnet_get_companion();
        Object* weapon = NULL;
        int weaponPid = atoi(w[2].c_str());
        int ammoPid = w.size() >= 4 ? atoi(w[3].c_str()) : -1;
        if (who == NULL || obj_pid_new(&weapon, weaponPid) == -1 || weapon == NULL || item_add_force(who, weapon, 1) != 0) {
            check_failed("wield " + w[1] + " pid " + w[2] + " failed");
        } else {
            obj_disconnect(weapon, NULL);
            if (ammoPid > 0) {
                weapon->data.item.weapon.ammoTypePid = ammoPid;
                weapon->data.item.weapon.ammoQuantity = 12;
            }
            inven_wield(who, weapon, 0); // left hand = the first slot, the one a fresh character fights with
            tile_refresh_display();
            check_passed("wield " + w[1] + " pid " + w[2]);
        }
    } else if (cmd == "movie" && w.size() >= 2) {
        // Play a cutscene by its number on this game (on the host the client plays it too).
        int rc = gmovie_play(atoi(w[1].c_str()), GAME_MOVIE_FADE_IN | GAME_MOVIE_FADE_OUT | GAME_MOVIE_STOP_MUSIC);
        tile_refresh_display();
        if (rc == 0) {
            check_passed("movie " + w[1]);
        } else {
            check_failed("movie " + w[1] + " could not be played");
        }
    } else if (cmd == "attack_enemy") {
        // Our own turn: the player's character attacks the nearest enemy with its current weapon.
        Object* target = NULL;
        int bestDistance = 1 << 30;
        for (Object* o = obj_find_first_at(map_elevation); o != NULL; o = obj_find_next_at()) {
            if (FID_TYPE(o->fid) != OBJ_TYPE_CRITTER || o == obj_dude || o == coopnet_get_companion() || o->tile == -1
                || critter_is_dead(o) || o->data.critter.combat.team == obj_dude->data.critter.combat.team) {
                continue;
            }
            int d = tile_dist(o->tile, obj_dude->tile);
            if (d < bestDistance) {
                bestDistance = d;
                target = o;
            }
        }
        if (target == NULL) {
            result_line("NOTE attack_enemy: no enemy left");
        } else {
            combat_attack_this(target);
            check_passed("attack_enemy pid " + std::to_string(target->pid));
        }
    } else if (cmd == "time" && w.size() >= 2) {
        // time <hours>: let game time pass (poison, radiation and other timed effects happen over game time).
        int hours = atoi(w[1].c_str());
        for (int h = 0; h < hours; h++) {
            inc_game_time(GAME_TIME_TICKS_PER_HOUR);
            queue_process();
        }
        check_passed("time +" + w[1] + " hours");
    } else if (cmd == "use_door") {
        // The player's character opens/closes the nearest door (a reach gesture, mirrored to the client).
        Object* door = NULL;
        int bestDistance = 1 << 30;
        for (Object* o = obj_find_first_at(map_elevation); o != NULL; o = obj_find_next_at()) {
            if (FID_TYPE(o->fid) != OBJ_TYPE_SCENERY || o->tile == -1 || (o->flags & OBJECT_HIDDEN) != 0) {
                continue;
            }
            Proto* proto = NULL;
            if (proto_ptr(o->pid, &proto) == -1 || proto == NULL || proto->scenery.type != SCENERY_TYPE_DOOR) {
                continue;
            }
            int d = tile_dist(o->tile, obj_dude->tile);
            if (d < bestDistance) {
                bestDistance = d;
                door = o;
            }
        }
        if (door == NULL) {
            check_failed("use_door: no door on this map");
        } else {
            action_use_an_object(obj_dude, door);
            check_passed("use_door at distance " + std::to_string(bestDistance));
        }
    } else if (cmd == "drop_item" && w.size() >= 2) {
        // An item on the ground next to the player's character.
        Object* item = NULL;
        int tile = tile_num_in_direction(obj_dude->tile, 3, 1);
        if (tile == -1 || obj_pid_new(&item, atoi(w[1].c_str())) == -1 || item == NULL) {
            check_failed("drop_item " + w[1] + " failed");
        } else {
            Rect rect;
            obj_move_to_tile(item, tile, obj_dude->elevation, &rect);
            tile_refresh_rect(&rect, obj_dude->elevation);
            check_passed("drop_item " + w[1]);
        }
    } else if (cmd == "pickup_item") {
        // The player's character picks up the nearest item lying on the ground (reach gesture + inventory change).
        Object* nearest = NULL;
        int bestDistance = 1 << 30;
        for (Object* o = obj_find_first_at(map_elevation); o != NULL; o = obj_find_next_at()) {
            if (FID_TYPE(o->fid) != OBJ_TYPE_ITEM || o->tile == -1 || (o->flags & OBJECT_HIDDEN) != 0) {
                continue;
            }
            int d = tile_dist(o->tile, obj_dude->tile);
            if (d < bestDistance) {
                bestDistance = d;
                nearest = o;
            }
        }
        if (nearest == NULL || bestDistance > 6) {
            check_failed("pickup_item: no item within reach");
        } else {
            action_get_an_object(obj_dude, nearest);
            check_passed("pickup_item at distance " + std::to_string(bestDistance));
        }
    } else if (cmd == "talk") {
        // talk [name part]: start a conversation with the nearest person (or the one whose name contains the text).
        std::string wanted = w.size() >= 2 ? rest_of_line(line, 1) : "";
        Object* person = NULL;
        int bestDistance = 1 << 30;
        for (Object* o = obj_find_first_at(map_elevation); o != NULL; o = obj_find_next_at()) {
            if (FID_TYPE(o->fid) != OBJ_TYPE_CRITTER || o == obj_dude || o == coopnet_get_companion() || o->tile == -1
                || (o->flags & OBJECT_HIDDEN) != 0 || critter_is_dead(o) || o->sid == -1
                || o->data.critter.combat.team != obj_dude->data.critter.combat.team * 0 + o->data.critter.combat.team) {
                continue;
            }
            if (!wanted.empty()) {
                const char* name = critter_name(o);
                if (name == NULL || std::string(name).find(wanted) == std::string::npos) {
                    continue;
                }
            }
            int d = tile_dist(o->tile, obj_dude->tile);
            if (d < bestDistance) {
                bestDistance = d;
                person = o;
            }
        }
        if (person == NULL) {
            check_failed("talk: nobody to talk to");
        } else {
            result_line("NOTE talk to %s (pid %d) at distance %d", critter_name(person) != NULL ? critter_name(person) : "?", person->pid, bestDistance);
            scripts_request_dialog(person);
            check_passed("talk");
        }
    } else if (cmd == "kill_party" || cmd == "list_party") {
        // Host: kill (or just list) the party NPCs that follow the host, e.g. Ian.
        int found = 0;
        for (Object* o = obj_find_first_at(map_elevation); o != NULL; o = obj_find_next_at()) {
            if (FID_TYPE(o->fid) != OBJ_TYPE_CRITTER || o == obj_dude || o == coopnet_get_companion() || o->tile == -1 || !isPartyMember(o)) {
                continue;
            }
            found++;
            result_line("NOTE party member pid %d tile %d dead %d", o->pid, o->tile, critter_is_dead(o) ? 1 : 0);
            if (cmd == "kill_party" && !critter_is_dead(o)) {
                critter_adjust_hits(o, -999);
            }
        }
        if (found == 0) {
            check_failed(cmd + ": no party member on this map");
        } else {
            check_passed(cmd);
        }
    } else if (cmd == "list_dead") {
        // Every dead critter on this map (a corpse of the friend's character where none should be shows up here).
        int dead = 0;
        int friendCorpses = 0;
        for (Object* o = obj_find_first_at(map_elevation); o != NULL; o = obj_find_next_at()) {
            if (FID_TYPE(o->fid) != OBJ_TYPE_CRITTER || o->tile == -1 || !critter_is_dead(o)) {
                continue;
            }
            dead++;
            bool friendLike = o->pid == 0x10003E7 || o->pid == 16778215 || o->pid == 16777216;
            if (friendLike && o != obj_dude && o != coopnet_get_companion()) {
                friendCorpses++;
            }
            result_line("DEAD pid=%d tile=%d fid=0x%x%s", o->pid, o->tile, o->fid, friendLike ? " <-- a player-character body" : "");
        }
        result_line("DEAD summary: %d dead critter(s), %d player-character body(ies)", dead, friendCorpses);
        if (friendCorpses == 0) {
            check_passed("no stray player-character bodies");
        } else {
            check_failed("a stray player-character body is on the map");
        }
    } else if (cmd == "caption") {
        // A caption for a recording: shown in the window's title bar and in the game's message pane.
        std::string text = rest_of_line(line, 1);
        if (gSdlWindow != NULL) {
            std::string title = "FALLOUT  -  " + text;
            SDL_SetWindowTitle(gSdlWindow, title.c_str());
        }
        char message[160];
        snprintf(message, sizeof(message), "%s", text.c_str());
        display_print(message);
        result_line("CAPTION %s", text.c_str());
    } else if (cmd == "called_shot" && w.size() >= 2) {
        // called_shot <head|left_arm|right_arm|torso|right_leg|left_leg|eyes|groin|off>: the body part an aimed shot hits.
        static const char* names[] = { "head", "left_arm", "right_arm", "torso", "right_leg", "left_leg", "eyes", "groin" };
        g_calledShot = -1;
        for (int i = 0; i < 8; i++) {
            if (w[1] == names[i]) {
                g_calledShot = i;
            }
        }
        check_passed("called_shot " + w[1]);
    } else if (cmd == "aim" && w.size() >= 2) {
        // aim on|off: switch the weapon to its aimed (called shot) mode, or back to a plain shot.
        bool wantAiming = w[1] == "on";
        for (int i = 0; i < 6; i++) {
            int hitMode;
            bool aiming = false;
            if (intface_get_attack(&hitMode, &aiming) == -1 || aiming == wantAiming) {
                break;
            }
            intface_toggle_item_state();
        }
        int hitMode;
        bool aimingNow = false;
        intface_get_attack(&hitMode, &aimingNow);
        if (aimingNow == wantAiming) {
            check_passed("aim " + w[1]);
        } else {
            check_failed("aim " + w[1] + " (this weapon has no aimed mode)");
        }
    } else if (cmd == "click_here") {
        int hx, hy;
        mouse_get_position(&hx, &hy);
        queue_click(hx, hy, MOUSE_STATE_LEFT_BUTTON_DOWN, false);
        g_waitUntil = now + 300;
    } else if (cmd == "quit") {
        finish(w.size() >= 2 ? atoi(w[1].c_str()) : (g_fail > 0 ? 1 : 0));
    } else {
        check_failed("unknown command: " + line);
    }

    if (g_abortOnFail && g_fail > 0) {
        finish(1);
    }
}

} // namespace

void autotest_parse_command_line(int argc, char** argv)
{
    for (int i = 0; i < argc; i++) {
        if (strncmp(argv[i], "--autotest=", 11) == 0) {
            g_scriptPath = argv[i] + 11;
        } else if (strncmp(argv[i], "--autotest-out=", 15) == 0) {
            g_outDir = argv[i] + 15;
        } else if (strncmp(argv[i], "--autotest-timeout=", 19) == 0) {
            g_timeoutMs = static_cast<uint32_t>(atoi(argv[i] + 19));
        } else if (strcmp(argv[i], "--autotest-hidden") == 0) {
            g_hidden = true;
        } else if (strncmp(argv[i], "--autotest-pos=", 15) == 0) {
            g_havePos = sscanf(argv[i] + 15, "%d,%d", &g_posX, &g_posY) == 2;
        }
    }

    if (g_scriptPath.empty()) {
        return;
    }

    FILE* script = fopen(g_scriptPath.c_str(), "rb");
    if (script == NULL) {
        fprintf(stderr, "autotest: cannot open %s\n", g_scriptPath.c_str());
        return;
    }
    std::string current;
    int c;
    while ((c = fgetc(script)) != EOF) {
        if (c == '\n') {
            g_lines.push_back(current);
            current.clear();
        } else {
            current.push_back(static_cast<char>(c));
        }
    }
    if (!current.empty()) {
        g_lines.push_back(current);
    }
    fclose(script);

    compat_mkdir(g_outDir.c_str());
    std::string resultPath = g_outDir + "/result.txt";
    g_result = fopen(resultPath.c_str(), "wt");
    g_active = true;
}

bool autotest_active()
{
    return g_active;
}

bool autotest_hidden()
{
    return g_active && g_hidden;
}

int autotest_called_shot()
{
    return g_active ? g_calledShot : -1;
}

bool autotest_window_position(int* x, int* y)
{
    if (!g_active || !g_havePos) {
        return false;
    }
    *x = g_posX;
    *y = g_posY;
    return true;
}

void autotest_note_debug(const char* text)
{
    if (!g_active || text == NULL) {
        return;
    }
    g_log.append(text);
    if (g_log.size() > 600000) {
        g_log.erase(0, 300000);
        g_logDropped += 300000;
    }
}

void autotest_tick()
{
    if (!g_active || (g_inTick && (g_commandDepth == 0 || g_commandDepth > 3))) {
        return;
    }
    bool wasInTick = g_inTick;
    g_inTick = true;

    if (g_godMode) {
        for (Object* who : { obj_dude, coopnet_get_companion() }) {
            if (who != NULL && who->data.critter.hp > 0 && who->data.critter.hp < 30) {
                who->data.critter.hp = 30;
            }
        }
    }

    uint32_t now = SDL_GetTicks();
    if (!g_started) {
        g_started = true;
        g_startMs = now;
        result_line("START script=%s lines=%d out=%s", g_scriptPath.c_str(), static_cast<int>(g_lines.size()), g_outDir.c_str());
    }

    if (now - g_startMs > g_timeoutMs) {
        check_failed("the script did not finish within the time limit");
        finish(2);
    }

    // One input step per tick, so a press and its release land on different frames.
    if (!g_steps.empty()) {
        if (g_stepDue == 0) {
            g_stepDue = now + g_steps.front().delayMs;
        }
        if (now >= g_stepDue) {
            apply_step(g_steps.front());
            g_steps.pop_front();
            g_stepDue = g_steps.empty() ? 0 : now + g_steps.front().delayMs;
        }
        g_inTick = wasInTick;
        return;
    }

    if (now < g_waitUntil) {
        g_inTick = wasInTick;
        return;
    }

    if (g_waitingFor) {
        if (log_contains_since(g_waitText, g_waitMark)) {
            g_waitingFor = false;
            check_passed("wait_for '" + g_waitText + "'");
        } else if (now >= g_waitDeadline) {
            g_waitingFor = false;
            check_failed("wait_for '" + g_waitText + "' (timed out)");
            if (g_abortOnFail) {
                finish(1);
            }
        } else {
            g_inTick = wasInTick;
            return;
        }
    }

    // The explore bot decides what to do next whenever it has nothing queued.
    if (g_exploreUntil != 0 && g_injected.empty()) {
        if (now >= g_exploreUntil) {
            g_exploreUntil = 0;
            explore_finish();
        } else if (now >= g_exploreNext) {
            g_commandDepth++;
            explore_decide(now);
            g_commandDepth--;
            g_exploreNext = now + 1200;
        }
    }

    while ((!g_injected.empty() || (g_exploreUntil == 0 && g_next < g_lines.size())) && g_steps.empty() && now >= g_waitUntil && !g_waitingFor) {
        std::string line;
        if (!g_injected.empty()) {
            line = g_injected.front();
            g_injected.pop_front();
        } else {
            line = g_lines[g_next++];
        }
        g_commandDepth++;
        execute_line(line, now);
        g_commandDepth--;
    }

    if (g_next >= g_lines.size() && g_exploreUntil == 0 && g_injected.empty() && g_steps.empty() && !g_waitingFor && now >= g_waitUntil) {
        finish(g_fail > 0 ? 1 : 0);
    }

    g_inTick = wasInTick;
}

} // namespace fallout
