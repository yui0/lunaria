/*
 * Copyright © 2026 Yuichiro Nakada / Project Lunaria
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

/*
 * Bridge to a local qfkey (qberry-core) process: publishes this emulator's
 * frames as a remote-desktop "window" and takes touch/key/pointer input back.
 *
 * Transport: a control socket (qfkey listens; lunaria connects, retrying) and
 * one shared-memory segment per lunaria holding a double-buffered frame.
 * Frames are produced ONLY while qfkey has subscribed (a viewer is looking),
 * so an unwatched lunaria pays no readback/copy cost.
 *
 * Wire/layout contract is mirrored in qberry-core/src/lunaria_bridge.rs.
 */
#ifndef LUNA_QFKEY_H
#define LUNA_QFKEY_H
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Host-UI (launcher) input: action 0=down 1=up 2=move; key is USB HID usage
 * and down is 0/1.  When set, pump_input delivers here instead of the guest. */
typedef void (*luna_qfkey_touch_fn)(int action, float x, float y);
typedef void (*luna_qfkey_key_fn)(int hid, int down);
void luna_qfkey_set_host_handlers(luna_qfkey_touch_fn touch, luna_qfkey_key_fn key);
/* Host-UI wheel: lines scrolled, positive y = content moves down (as GLFW's
 * scroll callback).  Only delivered while host handlers are set. */
typedef void (*luna_qfkey_scroll_fn)(double x, double y);
void luna_qfkey_set_host_scroll(luna_qfkey_scroll_fn scroll);

/* Starts the background connector if LUNARIA_QFKEY (or lunaria.conf qfkey) is
 * not "off".  Safe to call more than once.  When LUNARIA_INSTANCE is set and
 * title is empty or "lunaria", the window title becomes "lunaria-<instance>". */
void luna_qfkey_start(const char *title, int width, int height);

/* True while qfkey wants frames.  Cheap (one atomic load); call before any
 * costly readback. */
bool luna_qfkey_wants_frame(void);
/* Wake a sleeping host UI after remote input or background progress.
 * Callback runs under an internal lock; it must not call these helpers. */
void luna_qfkey_set_wake(void (*wake)(void));
void luna_qfkey_wake(void);
void luna_qfkey_wait(unsigned timeout_ms);

/* Pump thread, once per frame: delivers input received from qfkey to the
 * host handlers (if set) or the emulator's own queues. */
void luna_qfkey_pump_input(void);

/* Publish a finished frame: top row first, 4 bytes/pixel, stride in bytes. */
void luna_qfkey_publish(const void *pixels, int w, int h, int stride, bool bgra);
/* Present thread, before swap: capture the composed default framebuffer.
 * Does no readback unless subscribed; preserves the context's read/pack state. */
void luna_qfkey_capture_gl(int w, int h);

#ifdef __cplusplus
}
#endif
#endif
