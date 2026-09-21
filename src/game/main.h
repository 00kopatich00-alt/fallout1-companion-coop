#ifndef FALLOUT_GAME_MAIN_H_
#define FALLOUT_GAME_MAIN_H_

namespace fallout {

extern int main_game_paused;

int gnw_main(int argc, char** argv);

// Called from coopnet.cc when the shared coop game ends (either player's
// character effectively died) so the real death cutscene/narration plays on
// return to the main menu, the same as obj_dude's own death already
// triggers -- main_show_death_scene is otherwise file-static to main.cc.
void main_request_death_scene();

} // namespace fallout

#endif /* FALLOUT_GAME_MAIN_H_ */
