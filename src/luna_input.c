/* SPDX-License-Identifier: MPL-2.0 */
#include "luna_input.h"
#include <string.h>
int luna_event_count(const luna_touch_event *e)
{ return e ? (e->pointer_count ? e->pointer_count : 1) : 0; }
int luna_event_id(const luna_touch_event *e, int i)
{ return i >= 0 && i < luna_event_count(e) ? (e->pointer_count ? e->pointers[i].id : 0) : -1; }
float luna_event_x(const luna_touch_event *e, int i)
{ return i >= 0 && i < luna_event_count(e) ? (e->pointer_count ? e->pointers[i].x : e->x) : 0; }
float luna_event_y(const luna_touch_event *e, int i)
{ return i >= 0 && i < luna_event_count(e) ? (e->pointer_count ? e->pointers[i].y : e->y) : 0; }

int luna_touch_update(luna_touch_state *s, int id, int action, float x, float y,
                      long long now, luna_touch_event *out)
{
    int index = -1;
    if (!s || !out || id < 0 || id > 31 || action < 0 || action > 3) return 0;
    for (int i = 0; i < s->count; ++i) if (s->points[i].id == id) index = i;
    if (action == 3) {
        if (!s->count) return 0;
    } else if (action == 0) {
        if (index >= 0 || s->count >= LUNA_TOUCH_MAX) return 0;
        if (!s->count) s->down_ms = now;
        index = s->count++;
        s->points[index].id = id;
    } else if (index < 0) return 0;
    if (action != 3) {
        s->points[index].x = x;
        s->points[index].y = y;
    }
    memset(out, 0, sizeof *out);
    out->action = action;
    if (s->count > 1 && (action == 0 || action == 1))
        out->action = (action == 0 ? 5 : 6) | (index << 8);
    out->pointer_count = s->count;
    memcpy(out->pointers, s->points, (size_t)s->count * sizeof s->points[0]);
    out->x = s->points[0].x;
    out->y = s->points[0].y;
    out->event_ms = now;
    out->down_ms = s->down_ms;
    if (action == 1) {
        --s->count;
        memmove(&s->points[index], &s->points[index + 1],
                (size_t)(s->count - index) * sizeof s->points[0]);
    } else if (action == 3) s->count = 0;
    return 1;
}
