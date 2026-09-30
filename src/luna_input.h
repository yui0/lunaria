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
typedef struct luna_key_binding { int key, held; float x, y; } luna_key_binding;
typedef struct luna_keymap {
    int enabled, direction[4], stick_down, count;
    float stick_x, stick_y, radius;
    luna_key_binding buttons[LUNA_KEYMAP_BUTTONS];
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
#ifdef __cplusplus
}
#endif
#endif
