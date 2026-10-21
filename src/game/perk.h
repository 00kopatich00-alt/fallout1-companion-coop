#ifndef FALLOUT_GAME_PERK_H_
#define FALLOUT_GAME_PERK_H_

#include "game/object_types.h"
#include "game/perk_defs.h"
#include "plib/db/db.h"

namespace fallout {

int perk_init();
int perk_reset();
int perk_exit();
int perk_load(DB_FILE* stream);
int perk_save(DB_FILE* stream);
int perk_add(int perk);
int perk_sub(int perk);
int perk_make_list(int* perks);
int perk_level(int perk);
// Autotest only: grants a perk rank without the level and stat requirements.
int perk_add_forced(int perk);

// Coop: perks of a character that is not obj_dude. The companion (the client's character)
// plays on the host with the perks its player chose; everyone else has none.
bool perk_is_player(Object* critter);
int perk_level_of(Object* critter, int perk);
// Perks that act for the whole party (dialogue, travel, reactions): the better of both players.
int perk_level_any(int perk);
void perk_coop_set_companion(const int* levels, int count);
char* perk_name(int perk);
char* perk_description(int perk);
void perk_add_effect(Object* critter, int perk);
void perk_remove_effect(Object* critter, int perk);
int perk_adjust_skill(int skill);

} // namespace fallout

#endif /* FALLOUT_GAME_PERK_H_ */
