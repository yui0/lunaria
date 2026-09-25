/* Chrome DevTools Protocol pipe framing.  C11, with no browser or OS headers.
 * The platform launcher supplies byte-stream read/write callbacks. */
#ifndef LUNARIA_WEBVIEW_CDP_H
#define LUNARIA_WEBVIEW_CDP_H

#include <stddef.h>

typedef ptrdiff_t (*lunaria_cdp_read_fn)(void *context, void *dst, size_t size);
typedef ptrdiff_t (*lunaria_cdp_write_fn)(void *context, const void *src, size_t size);

struct lunaria_cdp {
   void *context;
   lunaria_cdp_read_fn read;
   lunaria_cdp_write_fn write;
   unsigned char *buffer;
   size_t used, capacity;
};

/* The caller owns the returned NUL-terminated JSON message and frees it. */
char *lunaria_cdp_receive(struct lunaria_cdp *cdp);
/* Nonblocking read: returns one owned message, or NULL. *status is 1 for a
 * complete message, 0 when more bytes are needed, and -1 on EOF/error. The
 * read callback must return -2 for would-block. */
char *lunaria_cdp_try_receive(struct lunaria_cdp *cdp, int *status);
int lunaria_cdp_send(struct lunaria_cdp *cdp, const char *json);
void lunaria_cdp_close(struct lunaria_cdp *cdp);

/* ---- one page over the pipe ------------------------------------------ */
#include <stddef.h>

struct lunaria_cdp_page {
   struct lunaria_cdp *transport;
   char session_id[128];
   unsigned next_id;
   char target_id[128];
   unsigned setup_id;
   int stage;
};

/* Creates and attaches to one browser page. These calls perform pipe I/O and
 * belong on a browser worker, never on the Android main looper. */
int lunaria_cdp_page_open(struct lunaria_cdp_page *page,
                          struct lunaria_cdp *transport);
/* Main-looper variant. The host must first enter nonblocking mode. poll()
 * returns 2 when setup finishes, 1 with an owned CDP message after setup,
 * 0 when no complete message is available, and -1 on transport/protocol error. */
int lunaria_cdp_page_begin(struct lunaria_cdp_page *page,
                           struct lunaria_cdp *transport);
int lunaria_cdp_page_poll(struct lunaria_cdp_page *page, char **message);
int lunaria_cdp_page_navigate(struct lunaria_cdp_page *page, const char *url);
int lunaria_cdp_page_capture(struct lunaria_cdp_page *page,
                             unsigned *request_id);
int lunaria_cdp_page_mouse(struct lunaria_cdp_page *page,
                           int x, int y, int pressed,
                           unsigned *request_id);
/* action: 1 down, -1 move, 0 up. */
int lunaria_cdp_page_touch(struct lunaria_cdp_page *page,
                           int x, int y, int action);
int lunaria_cdp_page_insert_text(struct lunaria_cdp_page *page,
                                 const char *text, unsigned *request_id);
int lunaria_cdp_page_key(struct lunaria_cdp_page *page,
                         const char *key, int virtual_code, int pressed);
int lunaria_cdp_page_eval(struct lunaria_cdp_page *page,
                          const char *expression, unsigned *request_id);
int lunaria_cdp_page_viewport(struct lunaria_cdp_page *page,
                              int width, int height);
/* width/height in CSS pixels; the page is rendered and captured at scale
 * device pixels per CSS pixel (Android: DisplayMetrics.density). */
int lunaria_cdp_page_viewport_scaled(struct lunaria_cdp_page *page,
                                     int width, int height, double scale);

/* Returns an owned CDP message. The worker must dispatch Page.loadEventFired,
 * frame events and other messages on the Android main looper as appropriate. */
char *lunaria_cdp_page_receive(struct lunaria_cdp_page *page);

/* Small JSON field helpers for CDP envelopes. A missing/malformed field fails.
 * String results are unescaped UTF-8. Duplicate keys return the first match. */
int lunaria_cdp_json_string(const char *json, const char *key,
                            char *out, size_t capacity);
int lunaria_cdp_json_integer(const char *json, const char *key, long *out);
/* The raw JSON text of a field's value (owned), e.g. for a RemoteObject's
 * "value" handed on as JSON. */
char *lunaria_cdp_json_raw(const char *json, const char *key);
/* A string field, fully unescaped (\uXXXX included); owned. */
char *lunaria_cdp_json_string_dup(const char *json, const char *key);
/* The same for a JSON string literal starting at p. */
char *lunaria_cdp_json_string_at(const char *p);
/* Any method on the page's session; params_json is an object (or NULL). */
int lunaria_cdp_page_command(struct lunaria_cdp_page *page, const char *method,
                             const char *params_json, unsigned *request_id);
char *lunaria_cdp_json_quote(const char *value);
/* Decodes the unescaped base64 "data" field of Page.captureScreenshot. */
unsigned char *lunaria_cdp_json_image(const char *json, size_t *length);

/* ---- the browser process (webview_cdp_host_<os>.c) --------------------- */
/* An installed Chromium-family browser, connected over its private CDP pipe.
 * Browser discovery belongs to the caller; the host never uses a personal
 * browser profile.  webview_cdp.h contains the platform-independent protocol.
 */


struct lunaria_cdp_host {
   struct lunaria_cdp cdp;
#ifdef _WIN32
   void *input_handle, *output_handle, *process_handle;
#else
   int input_fd, output_fd;
   long process_id;
#endif
   int nonblocking;
};

/* browser_binary must name the browser executable, not a shell wrapper which
 * may replace file descriptors 3 and 4.  profile_dir is a dedicated directory
 * for this emulated Android app.  Returns 0 on success. */
int lunaria_cdp_host_open(struct lunaria_cdp_host *host,
                          const char *browser_binary,
                          const char *profile_dir);
int lunaria_cdp_host_set_nonblocking(struct lunaria_cdp_host *host);
void lunaria_cdp_host_close(struct lunaria_cdp_host *host);

#endif
