#pragma once

// Plays an MP3 on a loop through SDL audio. Call music_update() once per frame.
int music_start(const char* path);
void music_update(void);
// Ramps the volume (0..1) over `seconds`.
void music_fade_to(float target, float seconds);
// True once a fade to 0 has finished.
int music_faded_out(void);
// Plays the "start game" chime over the music (not affected by music fades or the volume setting).
void music_sfx_launch(void);
// The player's volume choice, 0..1 (on top of fades).
void music_set_volume(float volume);
// Recent loudness 0..1 (drives the menu's little equalizer).
float music_level(void);
// Frees the audio device for a game, keeping the track position; music_resume fades back in from there.
void music_pause(void);
void music_resume(float fade_seconds);
void music_stop(void);
// Another track from its start, fading in.
int music_switch(const char* path);
