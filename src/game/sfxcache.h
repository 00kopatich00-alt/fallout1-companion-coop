#ifndef FALLOUT_GAME_SFXCACHE_H_
#define FALLOUT_GAME_SFXCACHE_H_

namespace fallout {

// The maximum number of sound effects that can be loaded and played
// simultaneously.
//
// Coop: was 4. The coop client now also plays every sound effect forwarded
// from the host's screen (barter/loot drag-drop, world map clicks) on top of
// its own local sounds, and a burst of quick clicks routinely piled up more
// than 4 short blips at once -- most silently failed to load ("already 4
// active effects", confirmed via debug log; roughly half of every rapid
// drag-drop). The underlying mixer supports up to AUDIO_ENGINE_SOUND_BUFFERS
// (8, audio_engine.h) concurrent buffers, so this still leaves headroom for
// background music/ambience.
#define SOUND_EFFECTS_MAX_COUNT 6

int sfxc_init(int cache_size, const char* effectsPath);
void sfxc_exit();
int sfxc_is_initialized();
void sfxc_flush();
int sfxc_cached_open(const char* fname, int mode);
int sfxc_cached_close(int handle);
int sfxc_cached_read(int handle, void* buf, unsigned int size);
int sfxc_cached_write(int handle, const void* buf, unsigned int size);
long sfxc_cached_seek(int handle, long offset, int origin);
long sfxc_cached_tell(int handle);
long sfxc_cached_file_size(int handle);

} // namespace fallout

#endif /* FALLOUT_GAME_SFXCACHE_H_ */
