/* SPDX-License-Identifier: MPL-2.0 */
#ifndef LUNA_KEYMAP_H
#define LUNA_KEYMAP_H
#ifdef __cplusplus
extern "C" {
#endif
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
