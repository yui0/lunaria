/* SPDX-License-Identifier: MPL-2.0 */
#include "luna_input.h"
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <pthread.h>
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

/* GLFW's stable public key values; the mapper has no GLFW dependency. */
static int key_code(const char *s)
{
    if (!strcmp(s, "SPACE")) return 32;
    if (!strcmp(s, "SHIFT")) return 340;
    if (s[0] && !s[1] && ((s[0] >= 'A' && s[0] <= 'Z') || (s[0] >= '0' && s[0] <= '9'))) return s[0];
    return -1;
}
static int unit(float v) { return isfinite(v) && v >= 0 && v <= 1; }
int luna_keymap_load(luna_keymap *m, const char *path)
{
    luna_keymap next = {0};
    if (!path || !*path || !strcmp(path, "off")) { *m = next; return 1; }
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[256], name[32], key[32], extra;
    int ok = 1, stick = 0;
    while (fgets(line, sizeof line, f)) {
        float x, y, radius;
        if (sscanf(line, " %31s", name) != 1 || name[0] == '#') continue;
        if (!strcmp(name, "stick")) {
            if (stick || sscanf(line, " %*s %f %f %f %c", &x, &y, &radius, &extra) != 3 ||
                !unit(x) || !unit(y) || !isfinite(radius) || radius <= 0 || radius > .5f) { ok = 0; break; }
            next.stick_x = x; next.stick_y = y; next.radius = radius; stick = 1;
        } else if (!strcmp(name, "button")) {
            if (next.count == LUNA_KEYMAP_BUTTONS || sscanf(line, " %*s %31s %f %f %c", key, &x, &y, &extra) != 3 ||
                key_code(key) < 0 || !unit(x) || !unit(y)) { ok = 0; break; }
            int code = key_code(key);
            if (code == 'W' || code == 'A' || code == 'S' || code == 'D') { ok = 0; break; }
            for (int i = 0; i < next.count; ++i) if (next.buttons[i].key == code) ok = 0;
            if (!ok) break;
            next.buttons[next.count++] = (luna_key_binding){code, 0, x, y};
        } else { ok = 0; break; }
    }
    if (ferror(f)) ok = 0;
    fclose(f);
    if (!ok || !stick) return 0;
    next.enabled = 1; *m = next; return 1;
}
static void stick_event(luna_keymap *m, float w, float h, luna_keymap_emit emit, void *ctx)
{
    float dx = (float)(m->direction[3] - m->direction[1]);
    float dy = (float)(m->direction[2] - m->direction[0]);
    int held = 0;
    for (int i = 0; i < 4; ++i) held |= m->direction[i];
    float x = m->stick_x * w, y = m->stick_y * h;
    if (!held) {
        if (m->stick_down) emit(ctx, 1, 1, x, y);
        m->stick_down = 0; return;
    }
    if (!m->stick_down) { emit(ctx, 1, 0, x, y); m->stick_down = 1; }
    float length = sqrtf(dx * dx + dy * dy);
    if (length > 0) { float r = m->radius * fminf(w, h); x += dx * r / length; y += dy * r / length; }
    emit(ctx, 1, 2, x, y);
}
int luna_keymap_key(luna_keymap *m, int key, int pressed, float w, float h, luna_keymap_emit emit, void *ctx)
{
    if (!m->enabled || w <= 0 || h <= 0) return 0;
    int shift_bit = key == 344 ? 2 : 1;
    if (key == 344) key = 340;
    const char *wasd = "WASD";
    for (int i = 0; i < 4; ++i) if (key == wasd[i]) {
        if (m->direction[i] != !!pressed) { m->direction[i] = !!pressed; stick_event(m, w, h, emit, ctx); }
        return 1;
    }
    for (int i = 0; i < m->count; ++i) if (key == m->buttons[i].key) {
        luna_key_binding *b = &m->buttons[i];
        int held = b->key == 340 ? (pressed ? b->held | shift_bit : b->held & ~shift_bit) : !!pressed;
        if (!!held != !!b->held) emit(ctx, i + 2, held ? 0 : 1, b->x * w, b->y * h);
        b->held = held;
        return 1;
    }
    return 0;
}
void luna_keymap_release(luna_keymap *m, float w, float h, luna_keymap_emit emit, void *ctx)
{
    memset(m->direction, 0, sizeof m->direction);
    stick_event(m, w, h, emit, ctx);
    for (int i = 0; i < m->count; ++i) if (m->buttons[i].held) {
        m->buttons[i].held = 0;
        emit(ctx, i + 2, 1, m->buttons[i].x * w, m->buttons[i].y * h);
    }
}

/* One process-wide selection, shared by the overlay and the input producer. */
static pthread_mutex_t selection_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t selection_once = PTHREAD_ONCE_INIT;
static luna_keymap selection;
static char selection_path[4096];
static int selection_pending;
static void selection_init(void)
{
    const char *path = getenv("LUNARIA_KEYMAP");
    if (!path || !*path) path = "off";
    if (strlen(path) < sizeof selection_path && luna_keymap_load(&selection, path))
        snprintf(selection_path, sizeof selection_path, "%s", path);
    else {
        strcpy(selection_path, "off");
        fprintf(stderr, "[input] invalid keymap: %s\n", path);
    }
    selection_pending = 1;
}
int luna_keymap_select(const char *path)
{
    luna_keymap next;
    if (!path || !*path) path = "off";
    if (strlen(path) >= sizeof selection_path || !luna_keymap_load(&next, path)) return 0;
    pthread_once(&selection_once, selection_init);
    pthread_mutex_lock(&selection_lock);
    selection = next;
    snprintf(selection_path, sizeof selection_path, "%s", path);
    selection_pending = 1;
    pthread_mutex_unlock(&selection_lock);
    return 1;
}
void luna_keymap_current(char *out, size_t cap)
{
    if (!out || !cap) return;
    pthread_once(&selection_once, selection_init);
    pthread_mutex_lock(&selection_lock);
    snprintf(out, cap, "%s", selection_path);
    pthread_mutex_unlock(&selection_lock);
}
int luna_keymap_take(luna_keymap *out)
{
    pthread_once(&selection_once, selection_init);
    pthread_mutex_lock(&selection_lock);
    int pending = selection_pending;
    if (pending) { *out = selection; selection_pending = 0; }
    pthread_mutex_unlock(&selection_lock);
    return pending;
}
