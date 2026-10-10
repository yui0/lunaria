/* SPDX-License-Identifier: MPL-2.0 */
#ifndef LUNA_INPUT_H
#define LUNA_INPUT_H
#ifdef __cplusplus
extern "C" {
#endif
#define LUNA_TOUCH_MAX 10
typedef struct luna_touch_point { int id; float x, y; } luna_touch_point;
typedef struct luna_touch_event {
    int action;
    float x, y; /* pointer zero, also used by legacy single-pointer producers */
    long long event_ms, down_ms;
    int pointer_count;
    luna_touch_point pointers[LUNA_TOUCH_MAX];
} luna_touch_event;
typedef struct luna_touch_state {
    int count;
    long long down_ms;
    luna_touch_point points[LUNA_TOUCH_MAX];
} luna_touch_state;
/* action: DOWN=0, UP=1, MOVE=2, CANCEL=3; id identifies a stable contact.
 * Returns a complete immutable Android event, including the lifting pointer
 * on POINTER_UP.  Pointer indices can change; pointer IDs do not. */
int luna_touch_update(luna_touch_state *, int id, int action, float x, float y,
                      long long now_ms, luna_touch_event *out);
int luna_event_count(const luna_touch_event *);
int luna_event_id(const luna_touch_event *, int index);
float luna_event_x(const luna_touch_event *, int index);
float luna_event_y(const luna_touch_event *, int index);
#include <stddef.h>
#define LUNA_KEYMAP_BUTTONS 8
#define LUNA_KEY_POINTS 4      /* contacts one key can press at the same time */
#define LUNA_KEYMAP_MACROS 8
#define LUNA_MACRO_STEPS 24
#define LUNA_TIMED_MAX 256
/* button KEY x y [x y ...] [repeat=MS] [hold=MS]
 *   Every point is pressed together (a simultaneous tap).  With repeat= the
 *   points are tapped again every MS milliseconds while the key is held, each
 *   tap lasting hold= milliseconds.  x/y mirror the first point. */
typedef struct luna_key_binding {
    int key, held;
    float x, y;
    int npoints, id, repeat_ms, hold_ms, phase;
    float px[LUNA_KEY_POINTS], py[LUNA_KEY_POINTS];
    long long cycle_ms;
} luna_key_binding;
/* macro KEY step... [loop]
 *   A step is "x,y[|x,y...][/hold_ms]" (taps those points together) or
 *   "@ms" (waits).  The sequence runs once per press; "loop" keeps running it
 *   while the key is held. */
typedef struct luna_macro_step {
    int npoints, hold_ms, wait_ms;
    float x[LUNA_KEY_POINTS], y[LUNA_KEY_POINTS];
} luna_macro_step;
typedef struct luna_macro {
    int key, held, loop, nsteps, id, pending;
    unsigned down;                 /* contacts currently pressed, by offset */
    luna_macro_step steps[LUNA_MACRO_STEPS];
} luna_macro;
typedef struct luna_timed { long long at_ms; int id, action, macro; float x, y; } luna_timed;
typedef struct luna_keymap {
    int enabled, direction[4], stick_down, count, nmacros, nqueue;
    float stick_x, stick_y, radius;
    int walk_key, walk_held;
    float walk_radius;
    luna_key_binding buttons[LUNA_KEYMAP_BUTTONS];
    luna_macro macros[LUNA_KEYMAP_MACROS];
    luna_timed queue[LUNA_TIMED_MAX];
} luna_keymap;
typedef void (*luna_keymap_emit)(void *, int id, int action, float x, float y);
/* Selection can come from the menu thread. The input pump takes a validated
 * replacement and releases old contacts before applying it. */
int luna_keymap_select(const char *path);
void luna_keymap_current(char *out, size_t capacity);
int luna_keymap_take(luna_keymap *out);
/* Profiles are ordinary normalized touch layouts; no guest-specific behavior. */
int luna_keymap_load(luna_keymap *, const char *profile);
int luna_keymap_key(luna_keymap *, int key, int pressed, float width, float height,
                    luna_keymap_emit, void *);
void luna_keymap_release(luna_keymap *, float width, float height, luna_keymap_emit, void *);

/* Everything that happens on a clock rather than a key event: repeat taps,
 * macro steps and the replay of a recording.  Call it regularly from the
 * thread that delivers keys (it emits through the same callback). */
void luna_input_tick(luna_keymap *, float width, float height, luna_keymap_emit, void *);
/* Tests substitute the clock (milliseconds, monotonic). */
void luna_input_set_clock(long long (*now_ms)(void));

/* Recording keeps every touch contact, whatever produced it, as text lines
 * "ms id action x y" with x,y as fractions of the view, so a recording replays
 * at any resolution.  Replay goes through the emit callback and is not itself
 * recorded.  luna_input_record_event is safe from any thread. */
int luna_record_start(const char *path);
void luna_record_stop(void);
int luna_record_active(void);
void luna_record_event(int id, int action, float nx, float ny);
int luna_play_start(const char *path, int loop);
void luna_play_stop(void);
int luna_play_active(void);
void luna_record_last_path(char *out, size_t capacity);
#ifdef __cplusplus
}
#endif
#endif
