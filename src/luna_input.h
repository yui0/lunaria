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
#ifdef __cplusplus
}
#endif
#endif
