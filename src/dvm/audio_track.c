/* MPL-2.0. The mixer owns copied PCM until it consumes the source frames. */
#include "audio_track.h"
#include "arm_exec.h"
#include <pthread.h>
#include <math.h>
#include <stdio.h>
#include <time.h>

struct luna_audio_track {
    struct luna_audio_track *next;
    struct luna_pcm_queue pcm;
    unsigned capacity, queued;
    uint64_t played;
    uint64_t clock_ns;  /* when the playback head was last advanced */
    int playing, logged;
    float volume;
};
static pthread_mutex_t tracks_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct luna_audio_track *tracks;
static pthread_cond_t tracks_changed=PTHREAD_COND_INITIALIZER;

static uint64_t now_ns(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts);
    return (uint64_t)ts.tv_sec*1000000000ull+(uint64_t)ts.tv_nsec;
}

static void clear_locked(struct luna_audio_track *t)
{
    for (unsigned n=0; n<t->pcm.count; ++n)
        free((void *)t->pcm.buffers[(t->pcm.head+n)%LUNA_PCM_BUFFERS].data);
    luna_pcm_clear(&t->pcm);
    t->queued=0;
}

struct luna_audio_track *luna_at_create(unsigned rate, unsigned channels, unsigned capacity)
{
    if (!rate || !channels || !capacity) return NULL;
    struct luna_audio_track *t=calloc(1,sizeof *t);
    if (!t) return NULL;
    t->pcm.rate=rate; t->pcm.channels=channels; t->pcm.capacity=LUNA_PCM_BUFFERS;
    t->capacity=capacity; t->volume=1.0f;
    pthread_mutex_lock(&tracks_mutex);
    t->next=tracks; tracks=t;
    pthread_mutex_unlock(&tracks_mutex);
    return t;
}

void luna_at_destroy(void *track)
{
    struct luna_audio_track *t=track;
    if (!t) return;
    pthread_mutex_lock(&tracks_mutex);
    struct luna_audio_track **p=&tracks;
    while (*p && *p!=t) p=&(*p)->next;
    if (*p) *p=t->next;
    clear_locked(t);
    pthread_mutex_unlock(&tracks_mutex);
    free(t);
}
void luna_at_play(struct luna_audio_track *t, int playing)
{
    if (!t) return;
    pthread_mutex_lock(&tracks_mutex); t->playing=playing; t->clock_ns=now_ns();
    pthread_cond_broadcast(&tracks_changed);
    pthread_mutex_unlock(&tracks_mutex);
}
void luna_at_flush(struct luna_audio_track *t)
{
    if (!t) return;
    pthread_mutex_lock(&tracks_mutex); clear_locked(t);
    pthread_cond_broadcast(&tracks_changed);
    pthread_mutex_unlock(&tracks_mutex);
}
void luna_at_volume(struct luna_audio_track *t, float volume)
{
    if (!t) return;
    pthread_mutex_lock(&tracks_mutex);
    t->volume=isfinite(volume) ? fmaxf(0.0f,fminf(1.0f,volume)) : 0.0f;
    pthread_mutex_unlock(&tracks_mutex);
}
unsigned luna_at_space(struct luna_audio_track *t)
{
    if (!t) return 0;
    pthread_mutex_lock(&tracks_mutex);
    unsigned n=t->pcm.count==LUNA_PCM_BUFFERS ? 0 : t->capacity-t->queued;
    pthread_mutex_unlock(&tracks_mutex);
    return n;
}
unsigned luna_at_write(struct luna_audio_track *t, const int16_t *data, unsigned frames)
{
    if (!t || !data || !frames) return 0;
    pthread_mutex_lock(&tracks_mutex);
    unsigned n=t->capacity-t->queued;
    if (n>frames) n=frames;
    if (t->pcm.count==LUNA_PCM_BUFFERS) n=0;
    unsigned bytes=n*t->pcm.channels*sizeof(int16_t);
    int16_t *copy=n ? malloc(bytes) : NULL;
    if (copy) {
        memcpy(copy,data,bytes);
        if (luna_pcm_enqueue(&t->pcm,copy,bytes)) { free(copy); n=0; }
        else {
            t->queued+=n;
            if (!t->logged) {
                unsigned peak=0;
                for (unsigned i=0; i<n*t->pcm.channels; ++i) {
                    unsigned value=(unsigned)(data[i]<0 ? -(int)data[i] : data[i]);
                    if (value>peak) peak=value;
                }
                if (peak) {
                    t->logged=1;
                    fprintf(stderr,"[audiotrack] PCM queued: %u Hz, %u channels, %u frames, peak=%u\n",
                            t->pcm.rate,t->pcm.channels,n,peak);
                }
            }
        }
    } else n=0;
    pthread_mutex_unlock(&tracks_mutex);
    return n;
}
uint64_t luna_at_played(struct luna_audio_track *t)
{
    if (!t) return 0;
    pthread_mutex_lock(&tracks_mutex); uint64_t n=t->played;
    pthread_mutex_unlock(&tracks_mutex); return n;
}
int luna_at_active(void)
{
    pthread_mutex_lock(&tracks_mutex);
    int active=0;
    for (struct luna_audio_track *t=tracks; t; t=t->next)
        if (t->playing && t->pcm.count) { active=1; break; }
    pthread_mutex_unlock(&tracks_mutex); return active;
}
unsigned luna_at_mix(int32_t *mix, unsigned frames, unsigned rate, unsigned channels)
{
    if (!mix || frames>256 || !channels || channels>2) return 0;
    pthread_mutex_lock(&tracks_mutex);
    unsigned produced=0;
    for (struct luna_audio_track *t=tracks; t; t=t->next) {
        if (!t->playing) continue;
        struct luna_pcm_queue preview=t->pcm;
        int32_t samples[256*2]={0};
        unsigned n=luna_pcm_mix(&preview,samples,frames,rate,channels);
        for (unsigned i=0; i<n*channels; ++i)
            mix[i]+=(int32_t)(samples[i]*t->volume);
        if (n>produced) produced=n;
    }
    pthread_mutex_unlock(&tracks_mutex); return produced;
}
/* Move one track's playback head forward by `frames` output frames at `rate`. */
static void advance_locked(struct luna_audio_track *t, unsigned frames,
                           unsigned rate, unsigned channels)
{
    unsigned head=t->pcm.head, old_count=t->pcm.count;
    unsigned old_position=old_count ? t->pcm.buffers[head].position : 0;
    luna_pcm_mix(&t->pcm,NULL,frames,rate,channels);
    unsigned retired=old_count-t->pcm.count;
    unsigned consumed=0;
    for (unsigned n=0; n<retired; ++n) {
        struct luna_pcm_buffer *b=&t->pcm.buffers[(head+n)%LUNA_PCM_BUFFERS];
        consumed+=b->frames;
        free((void *)b->data); b->data=NULL;
    }
    unsigned new_position=t->pcm.count ? t->pcm.buffers[t->pcm.head].position : 0;
    consumed+=new_position;
    consumed-=old_position;
    t->queued-=consumed;
    t->played+=consumed;
}

void luna_at_advance(unsigned frames, unsigned rate, unsigned channels)
{
    pthread_mutex_lock(&tracks_mutex);
    const uint64_t now=now_ns();
    for (struct luna_audio_track *t=tracks; t; t=t->next) {
        if (!t->playing) continue;
        advance_locked(t,frames,rate,channels);
        t->clock_ns=now;
    }
    pthread_cond_broadcast(&tracks_changed);
    pthread_mutex_unlock(&tracks_mutex);
}

/* A blocking write() waits for the device to drain the track.  On a device
 * that is the audio thread's job and never depends on the writer; here the
 * output mixer runs on the frame pump, which can itself be the writer (a
 * native audio engine calling AudioTrack.write from a JNI callback on the
 * pump).  Waiting for a consumer that is the waiter deadlocks the process, so
 * a writer that finds the head standing still plays the track out in real
 * time itself. */
void luna_at_wait_space(struct luna_audio_track *t)
{
    if (!t) return;
    enum { POLL_NS=10000000, STALL_NS=30000000 };
    pthread_mutex_lock(&tracks_mutex);
    while (t->playing && (t->queued==t->capacity || t->pcm.count==LUNA_PCM_BUFFERS)) {
        struct timespec until; clock_gettime(CLOCK_REALTIME,&until);
        until.tv_nsec+=POLL_NS;
        if (until.tv_nsec>=1000000000L) { until.tv_nsec-=1000000000L; ++until.tv_sec; }
        if (pthread_cond_timedwait(&tracks_changed,&tracks_mutex,&until)==0) continue;
        const uint64_t now=now_ns();
        if (now-t->clock_ns<STALL_NS) continue;
        uint64_t frames=(now-t->clock_ns)*t->pcm.rate/1000000000ull;
        if (frames>t->queued) frames=t->queued;
        t->clock_ns=now;
        if (frames) advance_locked(t,(unsigned)frames,t->pcm.rate,t->pcm.channels);
    }
    pthread_mutex_unlock(&tracks_mutex);
}
