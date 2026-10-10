/* MPL-2.0. Owned PCM queues shared by Java AudioTrack and the output mixer. */
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
struct luna_audio_track;
struct luna_audio_track *luna_at_create(unsigned rate, unsigned channels, unsigned capacity);
void luna_at_destroy(void *track);
void luna_at_play(struct luna_audio_track *track, int playing);
void luna_at_flush(struct luna_audio_track *track);
void luna_at_volume(struct luna_audio_track *track, float volume);
unsigned luna_at_write(struct luna_audio_track *track, const int16_t *pcm, unsigned frames);
void luna_at_wait_space(struct luna_audio_track *track);
unsigned luna_at_space(struct luna_audio_track *track);
uint64_t luna_at_played(struct luna_audio_track *track);
int luna_at_active(void);
unsigned luna_at_mix(int32_t *mix, unsigned frames, unsigned rate, unsigned channels);
void luna_at_advance(unsigned frames, unsigned rate, unsigned channels);
#ifdef __cplusplus
}
#endif
