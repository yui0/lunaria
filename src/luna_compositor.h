/*
 * Copyright © 2026 Yuichiro Nakada / Project Lunaria
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

/*
 * The host window's compositor — Lunaria's SurfaceFlinger.
 *
 * The guest never draws into the host window.  Its EGL "window" surface is a
 * pbuffer; eglSwapBuffers copies the finished frame into one of three shared
 * textures, fences it and returns.  A thread of the compositor's own is the
 * only thing that ever has the window surface current: it draws the newest
 * guest frame, puts the emulator's own UI (the boot card, dialogs, toasts, the
 * input method) over it, and swaps — at the display's pace, on its own
 * schedule.
 *
 * What that buys over compositing inside the guest's swap:
 *  - the emulator's UI is independent of the guest: the boot card keeps
 *    animating while every guest thread is inside a long JIT translation or
 *    blocked, and a keystroke into the input method is drawn without waiting
 *    for the game's next frame;
 *  - no guest thread ever waits for vsync inside eglSwapBuffers, and none is
 *    borrowed to draw the emulator's UI;
 *  - the window has exactly one producer, so there is nothing to alternate.
 *
 * eglSwapBuffers keeps a device's pacing: with a swap interval of one or more
 * it waits while a frame is already queued and not yet taken (a BufferQueue of
 * depth two), with zero it replaces the queued frame.
 */
#ifndef LUNARIA_LUNA_COMPOSITOR_H
#define LUNARIA_LUNA_COMPOSITOR_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Starts the compositor on `native_window`.  The compositor creates its own
 * context — in a share group of its own, apart from the guest's — and its own
 * window surface; the caller must not make that window current anywhere.
 * False when it could not start — the caller then presents the way it did
 * without one. */
bool luna_comp_start(void *egl_display, void *egl_config, void *native_window);

/* True once started and running. */
bool luna_comp_active(void);

/* Guest side, on the thread and context that just finished the frame, with
 * the frame in the default framebuffer of the current draw surface (w x h).
 * Copies it into a free slot and queues it.  `swap_interval` is the guest's
 * eglSwapInterval. */
void luna_comp_submit(int w, int h, int swap_interval);

/* A frame that is already in host memory: `w` x `h` pixels of four bytes,
 * rows `stride` bytes apart, top row first (Vulkan's order).  `bgra` says the
 * bytes are B,G,R,A rather than R,G,B,A.  Any thread; the pixels are copied
 * before this returns.  With `swap_interval` 1 or more it waits, bounded,
 * while the previous frame has not been taken yet. */
void luna_comp_submit_pixels(int w, int h, const void *pixels, int stride,
                             bool bgra, int swap_interval);

/* Something the compositor draws besides the guest's frame changed (a
 * document, a keystroke, a pointer event): draw a frame soon. */
void luna_comp_wake(void);

/* Frames the compositor has put on screen. */
unsigned long long luna_comp_frames(void);

#ifdef __cplusplus
}
#endif
#endif
