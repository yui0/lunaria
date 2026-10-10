/* SPDX-License-Identifier: MPL-2.0 */
/*
 * Bridge to a local qfkey (qberry-core) process: publishes this emulator's
 * frames as a remote-desktop "window" and takes touch/key input back.
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

/* Starts the background connector if LUNARIA_QFKEY (or lunaria.conf qfkey) is
 * not "off".  Safe to call more than once. */
void luna_qfkey_start(const char *title, int width, int height);

/* True while qfkey wants frames.  Cheap (one atomic load); call before any
 * costly readback. */
bool luna_qfkey_wants_frame(void);

/* Pump thread, once per frame: delivers input received from qfkey to the
 * emulator's own queues (they are not safe to touch from other threads). */
void luna_qfkey_pump_input(void);

/* Publish a finished frame: top row first, 4 bytes/pixel, stride in bytes. */
void luna_qfkey_publish(const void *pixels, int w, int h, int stride, bool bgra);

#ifdef __cplusplus
}
#endif
#endif
