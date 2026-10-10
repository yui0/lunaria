/* SPDX-License-Identifier: MPL-2.0 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "luna_input.h"
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <pthread.h>
#include <time.h>
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
    if (!strcmp(s, "CTRL")) return 341;
    if (s[0] && !s[1] && ((s[0] >= 'A' && s[0] <= 'Z') || (s[0] >= '0' && s[0] <= '9'))) return s[0];
    return -1;
}
static int unit(float v) { return isfinite(v) && v >= 0 && v <= 1; }

/* Tokens of one keymap line, split on blanks (the line is modified). */
static int split(char *line, char **tok, int max)
{
    int n = 0;
    for (char *p = strtok(line, " \t\r\n"); p && n < max; p = strtok(NULL, " \t\r\n")) tok[n++] = p;
    return n;
}
static int number(const char *s, float *v)
{
    char *end;
    *v = strtof(s, &end);
    return end != s && !*end && isfinite(*v);
}
static int whole(const char *s, int lo, int hi, int *v)
{
    char *end;
    long n = strtol(s, &end, 10);
    if (end == s || *end || n < lo || n > hi) return 0;
    *v = (int)n;
    return 1;
}
/* Ids: 0 is the mouse, 1 the stick, then one per contact a key can press. */
static int claim_ids(luna_keymap *m, int n)
{
    int next = 2;
    for (int i = 0; i < m->count; ++i) next += m->buttons[i].npoints;
    for (int i = 0; i < m->nmacros; ++i) next += LUNA_KEY_POINTS;
    return next + n <= 32 ? next : -1;
}
static int key_taken(const luna_keymap *m, int code)
{
    if (code == 'W' || code == 'A' || code == 'S' || code == 'D') return 1;
    for (int i = 0; i < m->count; ++i) if (m->buttons[i].key == code) return 1;
    for (int i = 0; i < m->nmacros; ++i) if (m->macros[i].key == code) return 1;
    return 0;
}
static int parse_button(luna_keymap *m, char *line)
{
    char *t[16];
    int n = split(line, t, 16), code, np = 0, repeat = 0, hold = 0;
    float v[2 * LUNA_KEY_POINTS];
    if (n < 4 || (code = key_code(t[1])) < 0 || key_taken(m, code)) return 0;
    for (int i = 2; i < n; ++i) {
        if (!strncmp(t[i], "repeat=", 7)) { if (!whole(t[i] + 7, 10, 60000, &repeat)) return 0; }
        else if (!strncmp(t[i], "hold=", 5)) { if (!whole(t[i] + 5, 1, 60000, &hold)) return 0; }
        else {
            if (np >= 2 * LUNA_KEY_POINTS || !number(t[i], &v[np]) || !unit(v[np])) return 0;
            ++np;
        }
    }
    if (np < 2 || (np & 1)) return 0;
    if (!hold) hold = repeat && repeat / 2 < 30 ? repeat / 2 : 30;
    if (repeat && hold >= repeat) return 0;
    int id = claim_ids(m, np / 2);
    if (id < 0) return 0;
    luna_key_binding *b = &m->buttons[m->count++];
    memset(b, 0, sizeof *b);
    b->key = code; b->npoints = np / 2; b->id = id; b->repeat_ms = repeat; b->hold_ms = hold;
    for (int i = 0; i < b->npoints; ++i) { b->px[i] = v[2 * i]; b->py[i] = v[2 * i + 1]; }
    b->x = b->px[0]; b->y = b->py[0];
    return 1;
}
static int parse_macro(luna_keymap *m, char *line)
{
    char *t[2 + LUNA_MACRO_STEPS + 1];
    int n = split(line, t, 2 + LUNA_MACRO_STEPS + 1), code;
    if (n < 3 || (code = key_code(t[1])) < 0 || key_taken(m, code)) return 0;
    int id = claim_ids(m, LUNA_KEY_POINTS);
    if (id < 0) return 0;
    luna_macro *mc = &m->macros[m->nmacros];
    memset(mc, 0, sizeof *mc);
    mc->key = code; mc->id = id;
    for (int i = 2; i < n; ++i) {
        if (!strcmp(t[i], "loop")) { mc->loop = 1; continue; }
        if (mc->nsteps == LUNA_MACRO_STEPS) return 0;
        luna_macro_step *st = &mc->steps[mc->nsteps++];
        if (t[i][0] == '@') { if (!whole(t[i] + 1, 1, 600000, &st->wait_ms)) return 0; continue; }
        st->hold_ms = 30;
        char *slash = strchr(t[i], '/');
        if (slash) { *slash = 0; if (!whole(slash + 1, 1, 60000, &st->hold_ms)) return 0; }
        for (char *pt = strtok(t[i], "|"); pt; pt = strtok(NULL, "|")) {
            char *comma = strchr(pt, ',');
            if (!comma || st->npoints == LUNA_KEY_POINTS) return 0;
            *comma = 0;
            if (!number(pt, &st->x[st->npoints]) || !number(comma + 1, &st->y[st->npoints]) ||
                !unit(st->x[st->npoints]) || !unit(st->y[st->npoints])) return 0;
            ++st->npoints;
        }
        if (!st->npoints) return 0;
    }
    ++m->nmacros;
    return 1;
}
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
        } else if (!strcmp(name, "walk")) {
            if (next.walk_key || sscanf(line, " %*s %31s %f %c", key, &radius, &extra) != 2 ||
                key_code(key) < 0 || !isfinite(radius) || radius <= 0 || radius > .5f) { ok = 0; break; }
            next.walk_key = key_code(key); next.walk_radius = radius;
            if (strchr("WASD", next.walk_key)) { ok = 0; break; }
        } else if (!strcmp(name, "button")) {
            if (next.count == LUNA_KEYMAP_BUTTONS || !parse_button(&next, line)) { ok = 0; break; }
        } else if (!strcmp(name, "macro")) {
            if (next.nmacros == LUNA_KEYMAP_MACROS || !parse_macro(&next, line)) { ok = 0; break; }
        } else { ok = 0; break; }
    }
    if (ferror(f)) ok = 0;
    fclose(f);
    if (!ok || !stick) return 0;
    if (next.walk_key) {
        if (next.walk_radius >= next.radius) return 0;
        for (int i = 0; i < next.count; ++i)
            if (next.buttons[i].key == next.walk_key) return 0;
    }
    next.enabled = 1; *m = next; return 1;
}

static long long (*clock_fn)(void);
static long long clock_ms(void)
{
    if (clock_fn) return clock_fn();
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
void luna_input_set_clock(long long (*now_ms)(void)) { clock_fn = now_ms; }

static int queue_put(luna_keymap *m, long long at, int id, int action, float x, float y, int macro)
{
    m->queue[m->nqueue++] = (luna_timed){at, id, action, macro, x, y};
    return 1;
}
/* Lays a macro's whole timeline down at once; nothing is queued unless it fits. */
static void macro_start(luna_keymap *m, int index, long long now)
{
    luna_macro *mc = &m->macros[index];
    int need = 0;
    for (int i = 0; i < mc->nsteps; ++i) need += 2 * mc->steps[i].npoints;
    if (!need || m->nqueue + need > LUNA_TIMED_MAX) return;
    long long t = now;
    for (int i = 0; i < mc->nsteps; ++i) {
        const luna_macro_step *st = &mc->steps[i];
        if (!st->npoints) { t += st->wait_ms; continue; }
        for (int p = 0; p < st->npoints; ++p) queue_put(m, t, mc->id + p, 0, st->x[p], st->y[p], index);
        t += st->hold_ms;
        for (int p = st->npoints - 1; p >= 0; --p) queue_put(m, t, mc->id + p, 1, st->x[p], st->y[p], index);
    }
    mc->pending = need;
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
    if (length > 0) { float r = (m->walk_held ? m->walk_radius : m->radius) * fminf(w, h); x += dx * r / length; y += dy * r / length; }
    emit(ctx, 1, 2, x, y);
}
int luna_keymap_key(luna_keymap *m, int key, int pressed, float w, float h, luna_keymap_emit emit, void *ctx)
{
    if (!m->enabled || w <= 0 || h <= 0) return 0;
    int shift_bit = key == 344 ? 2 : 1;
    if (key == 344) key = 340;
    int ctrl_bit = key == 345 ? 2 : 1;
    if (key == 345) key = 341;
    if (m->walk_key && key == m->walk_key) {
        int bit = key == 340 ? shift_bit : ctrl_bit;
        int held = key == 340 || key == 341 ? (pressed ? m->walk_held | bit : m->walk_held & ~bit) : !!pressed;
        if (!!held != !!m->walk_held) { m->walk_held = held; stick_event(m, w, h, emit, ctx); }
        else m->walk_held = held;
        return 1;
    }
    const char *wasd = "WASD";
    for (int i = 0; i < 4; ++i) if (key == wasd[i]) {
        if (m->direction[i] != !!pressed) { m->direction[i] = !!pressed; stick_event(m, w, h, emit, ctx); }
        return 1;
    }
    for (int i = 0; i < m->count; ++i) if (key == m->buttons[i].key) {
        luna_key_binding *b = &m->buttons[i];
        int bit = key == 340 ? shift_bit : ctrl_bit;
        int held = key == 340 || key == 341 ? (pressed ? b->held | bit : b->held & ~bit) : !!pressed;
        if (!!held != !!b->held) {
            if (held) {
                b->cycle_ms = clock_ms();
                for (int p = 0; p < b->npoints; ++p) emit(ctx, b->id + p, 0, b->px[p] * w, b->py[p] * h);
                b->phase = 1;
            } else if (b->phase) {
                for (int p = b->npoints - 1; p >= 0; --p) emit(ctx, b->id + p, 1, b->px[p] * w, b->py[p] * h);
                b->phase = 0;
            }
        }
        b->held = held;
        return 1;
    }
    for (int i = 0; i < m->nmacros; ++i) if (key == m->macros[i].key) {
        luna_macro *mc = &m->macros[i];
        mc->held = !!pressed;
        if (pressed && !mc->pending) macro_start(m, i, clock_ms());
        return 1;
    }
    return 0;
}
void luna_keymap_release(luna_keymap *m, float w, float h, luna_keymap_emit emit, void *ctx)
{
    memset(m->direction, 0, sizeof m->direction);
    m->walk_held = 0;
    stick_event(m, w, h, emit, ctx);
    for (int i = 0; i < m->count; ++i) {
        luna_key_binding *b = &m->buttons[i];
        if (b->held && b->phase)
            for (int p = b->npoints - 1; p >= 0; --p) emit(ctx, b->id + p, 1, b->px[p] * w, b->py[p] * h);
        b->held = b->phase = 0;
    }
    for (int i = 0; i < m->nmacros; ++i) {
        luna_macro *mc = &m->macros[i];
        for (int p = 0; p < LUNA_KEY_POINTS; ++p)
            if (mc->down & (1u << p)) emit(ctx, mc->id + p, 1, 0, 0);
        mc->down = 0; mc->held = 0; mc->pending = 0;
    }
    m->nqueue = 0;
}


/* ---- recording and replay ------------------------------------------------ */
static pthread_mutex_t rec_lock = PTHREAD_MUTEX_INITIALIZER;
static FILE *rec_file;
static long long rec_start_ms;
static char rec_last[4096];
static __thread int replaying;   /* replayed contacts are not recorded again */
struct play_event { long long ms; int id, action; float x, y; };
static struct play_event *play_events;
static int play_count, play_next, play_loop, play_stop;
static long long play_start_ms;
static unsigned play_down;       /* ids 0..31 currently pressed by the replay */

int luna_record_start(const char *path)
{
    if (!path || !*path) return 0;
    pthread_mutex_lock(&rec_lock);
    if (rec_file) fclose(rec_file);
    rec_file = fopen(path, "w");
    if (rec_file) {
        fputs("# lunaria-input 1: ms id action(0 down,1 up,2 move,3 cancel) x y (fractions of the view)\n", rec_file);
        rec_start_ms = clock_ms();
        snprintf(rec_last, sizeof rec_last, "%s", path);
    }
    int ok = rec_file != NULL;
    pthread_mutex_unlock(&rec_lock);
    return ok;
}
void luna_record_stop(void)
{
    pthread_mutex_lock(&rec_lock);
    if (rec_file) { fclose(rec_file); rec_file = NULL; }
    pthread_mutex_unlock(&rec_lock);
}
int luna_record_active(void)
{
    pthread_mutex_lock(&rec_lock);
    int on = rec_file != NULL;
    pthread_mutex_unlock(&rec_lock);
    return on;
}
void luna_record_event(int id, int action, float nx, float ny)
{
    if (replaying) return;
    pthread_mutex_lock(&rec_lock);
    if (rec_file)
        fprintf(rec_file, "%lld %d %d %.6f %.6f\n", clock_ms() - rec_start_ms, id, action, nx, ny);
    pthread_mutex_unlock(&rec_lock);
}
void luna_record_last_path(char *out, size_t cap)
{
    if (!out || !cap) return;
    pthread_mutex_lock(&rec_lock);
    snprintf(out, cap, "%s", rec_last);
    pthread_mutex_unlock(&rec_lock);
}
int luna_play_start(const char *path, int loop)
{
    FILE *f = path && *path ? fopen(path, "r") : NULL;
    if (!f) return 0;
    struct play_event *ev = NULL;
    int n = 0, cap = 0, ok = 1;
    char line[256];
    long long last = 0;
    while (fgets(line, sizeof line, f)) {
        struct play_event e;
        if (line[0] == '#' || line[0] == '\n') continue;
        if (sscanf(line, "%lld %d %d %f %f", &e.ms, &e.id, &e.action, &e.x, &e.y) != 5 ||
            e.ms < last || e.id < 0 || e.id > 31 || e.action < 0 || e.action > 3 ||
            !isfinite(e.x) || !isfinite(e.y) || n >= 200000) { ok = 0; break; }
        if (n == cap) {
            struct play_event *grown = realloc(ev, (size_t)(cap ? cap * 2 : 256) * sizeof *ev);
            if (!grown) { ok = 0; break; }
            ev = grown; cap = cap ? cap * 2 : 256;
        }
        last = e.ms; ev[n++] = e;
    }
    fclose(f);
    if (!ok || !n) { free(ev); return 0; }
    pthread_mutex_lock(&rec_lock);
    free(play_events);
    play_events = ev; play_count = n; play_next = 0; play_loop = !!loop; play_stop = 0;
    play_start_ms = clock_ms();
    snprintf(rec_last, sizeof rec_last, "%s", path);
    pthread_mutex_unlock(&rec_lock);
    return 1;
}
void luna_play_stop(void)
{
    pthread_mutex_lock(&rec_lock);
    if (play_events) play_stop = 1;     /* the tick releases what is held */
    pthread_mutex_unlock(&rec_lock);
}
int luna_play_active(void)
{
    pthread_mutex_lock(&rec_lock);
    int on = play_events != NULL;
    pthread_mutex_unlock(&rec_lock);
    return on;
}
static void play_tick(float w, float h, luna_keymap_emit emit, void *ctx)
{
    struct play_event due[64];
    int n = 0, finished = 0, release = 0;
    long long now = clock_ms();
    pthread_mutex_lock(&rec_lock);
    if (!play_events) { pthread_mutex_unlock(&rec_lock); return; }
    if (play_stop) { release = finished = 1; }
    else {
        while (play_next < play_count && n < 64 && play_events[play_next].ms <= now - play_start_ms)
            due[n++] = play_events[play_next++];
        if (play_next == play_count) {
            if (play_loop) { play_next = 0; play_start_ms = now; release = 1; }
            else finished = 1;
        }
    }
    if (finished) { free(play_events); play_events = NULL; play_count = 0; }
    pthread_mutex_unlock(&rec_lock);
    replaying = 1;
    for (int i = 0; i < n; ++i) {
        if (due[i].action == 0) play_down |= 1u << due[i].id;
        else if (due[i].action == 1 || due[i].action == 3) play_down &= due[i].action == 3 ? 0u : ~(1u << due[i].id);
        emit(ctx, due[i].id, due[i].action, due[i].x * w, due[i].y * h);
    }
    if (release || finished)
        for (int id = 0; id < 32; ++id) if (play_down & (1u << id)) emit(ctx, id, 1, 0, 0);
    if (release || finished) play_down = 0;
    replaying = 0;
}

void luna_input_tick(luna_keymap *m, float w, float h, luna_keymap_emit emit, void *ctx)
{
    long long now = clock_ms();
    if (m && m->enabled && w > 0 && h > 0) {
        for (int i = 0; i < m->count; ++i) {
            luna_key_binding *b = &m->buttons[i];
            if (!b->repeat_ms || !b->held) continue;
            if (b->phase && now - b->cycle_ms >= b->hold_ms) {
                for (int p = b->npoints - 1; p >= 0; --p) emit(ctx, b->id + p, 1, b->px[p] * w, b->py[p] * h);
                b->phase = 0;
            }
            if (!b->phase && now - b->cycle_ms >= b->repeat_ms) {
                /* A stalled tick resumes from now rather than bursting. */
                b->cycle_ms = now - b->cycle_ms >= 2 * b->repeat_ms ? now : b->cycle_ms + b->repeat_ms;
                for (int p = 0; p < b->npoints; ++p) emit(ctx, b->id + p, 0, b->px[p] * w, b->py[p] * h);
                b->phase = 1;
            }
        }
        for (;;) {
            int best = -1;
            for (int i = 0; i < m->nqueue; ++i)
                if (m->queue[i].at_ms <= now && (best < 0 || m->queue[i].at_ms < m->queue[best].at_ms)) best = i;
            if (best < 0) break;
            luna_timed e = m->queue[best];
            memmove(&m->queue[best], &m->queue[best + 1], (size_t)(--m->nqueue - best) * sizeof e);
            luna_macro *mc = &m->macros[e.macro];
            unsigned bit = 1u << (e.id - mc->id);
            if (e.action == 0) mc->down |= bit; else mc->down &= ~bit;
            --mc->pending;
            emit(ctx, e.id, e.action, e.x * w, e.y * h);
        }
        for (int i = 0; i < m->nmacros; ++i)
            if (m->macros[i].loop && m->macros[i].held && !m->macros[i].pending) macro_start(m, i, now);
    }
    play_tick(w, h, emit, ctx);
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
