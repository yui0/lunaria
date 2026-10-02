/* SPDX-License-Identifier: MPL-2.0 */
#ifndef LUNA_HOST_H
#define LUNA_HOST_H
#include <stddef.h>
#include <stdint.h>
#ifdef _WIN32
#include <winsock2.h>
#else
#include <poll.h>
#endif
#ifdef __cplusplus
extern "C" {
#endif
/* One host readiness source. Callback executes without the monitor lock. */
int luna_fd_monitor_start(void (*ready)(void));
int luna_fd_monitor_update(const struct pollfd *fds, size_t count, int64_t deadline_ms);
void luna_fd_monitor_stop(void);
/* Diagnostic GL state dump around draws; see luna_host.c. */
void luna_gl_inspect_draw(uint64_t frame);
#ifdef __cplusplus
}
#endif
#endif
