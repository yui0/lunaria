#include "webview_cdp.h"

#include <ctype.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Chromium's --remote-debugging-pipe uses NUL-terminated JSON messages.
 * Screenshot frames can be large; cap one message so a broken peer cannot
 * grow this buffer without bound. */
#define CDP_MAX_MESSAGE (32u * 1024u * 1024u)

int lunaria_cdp_send(struct lunaria_cdp *cdp, const char *json)
{
   if (!cdp || !cdp->write || !json) return -1;
   const size_t length = strlen(json);
   if (length >= CDP_MAX_MESSAGE) return -1;
   size_t sent = 0;
   while (sent <= length) {
      const ptrdiff_t n = cdp->write(cdp->context, json + sent,
                                      length + 1 - sent);
      if (n <= 0 || (size_t)n > length + 1 - sent) return -1;
      sent += (size_t)n;
   }
   return 0;
}

char *lunaria_cdp_receive(struct lunaria_cdp *cdp)
{
   if (!cdp || !cdp->read) return NULL;
   for (;;) {
      const unsigned char *end = cdp->buffer
         ? memchr(cdp->buffer, 0, cdp->used) : NULL;
      if (end) {
         const size_t length = (size_t)(end - cdp->buffer);
         char *message = malloc(length + 1);
         if (!message) return NULL;
         memcpy(message, cdp->buffer, length + 1);
         const size_t consumed = length + 1;
         cdp->used -= consumed;
         memmove(cdp->buffer, cdp->buffer + consumed, cdp->used);
         return message;
      }
      if (cdp->used >= CDP_MAX_MESSAGE) return NULL;
      if (cdp->capacity - cdp->used < 4096) {
         size_t next = cdp->capacity ? cdp->capacity * 2 : 4096;
         if (next > CDP_MAX_MESSAGE) next = CDP_MAX_MESSAGE;
         unsigned char *grown = realloc(cdp->buffer, next);
         if (!grown) return NULL;
         cdp->buffer = grown;
         cdp->capacity = next;
      }
      const ptrdiff_t n = cdp->read(cdp->context,
                                    cdp->buffer + cdp->used,
                                    cdp->capacity - cdp->used);
      if (n <= 0 || (size_t)n > cdp->capacity - cdp->used) return NULL;
      cdp->used += (size_t)n;
   }
}

char *lunaria_cdp_try_receive(struct lunaria_cdp *cdp, int *status)
{
   if (status) *status = -1;
   if (!cdp || !cdp->read || !status) return NULL;
   for (;;) {
      const unsigned char *end = cdp->buffer
         ? memchr(cdp->buffer, 0, cdp->used) : NULL;
      if (end) {
         const size_t length = (size_t)(end - cdp->buffer);
         char *message = malloc(length + 1);
         if (!message) return NULL;
         memcpy(message, cdp->buffer, length + 1);
         const size_t consumed = length + 1;
         cdp->used -= consumed;
         memmove(cdp->buffer, cdp->buffer + consumed, cdp->used);
         *status = 1;
         return message;
      }
      if (cdp->used >= CDP_MAX_MESSAGE) return NULL;
      if (cdp->capacity - cdp->used < 4096) {
         size_t next = cdp->capacity ? cdp->capacity * 2 : 4096;
         if (next > CDP_MAX_MESSAGE) next = CDP_MAX_MESSAGE;
         unsigned char *grown = realloc(cdp->buffer, next);
         if (!grown) return NULL;
         cdp->buffer = grown;
         cdp->capacity = next;
      }
      const ptrdiff_t n = cdp->read(cdp->context,
                                    cdp->buffer + cdp->used,
                                    cdp->capacity - cdp->used);
      if (n == -2) { *status = 0; return NULL; }
      if (n <= 0 || (size_t)n > cdp->capacity - cdp->used) return NULL;
      cdp->used += (size_t)n;
   }
}

void lunaria_cdp_close(struct lunaria_cdp *cdp)
{
   if (!cdp) return;
   free(cdp->buffer);
   cdp->buffer = NULL;
   cdp->used = cdp->capacity = 0;
}

/* ---- one page over the pipe ------------------------------------------ */

/* CDP envelopes are JSON. Walk strings structurally so an escaped quote or a
 * key in a string value cannot be mistaken for an envelope field. */
static const char *json_string_end(const char *p)
{
   if (*p++ != '"') return NULL;
   while (*p) {
      if (*p == '\\') {
         if (!p[1]) return NULL;
         p += 2;
      } else if (*p++ == '"') {
         return p;
      }
   }
   return NULL;
}

static const char *json_field(const char *json, const char *key)
{
   if (!json || !key) return NULL;
   const size_t key_len = strlen(key);
   for (const char *p = json; *p;) {
      if (*p != '"') { ++p; continue; }
      const char *end = json_string_end(p);
      if (!end) return NULL;
      const char *after = end;
      while (isspace((unsigned char)*after)) ++after;
      if ((size_t)(end - p - 2) == key_len &&
          memcmp(p + 1, key, key_len) == 0 && *after == ':') {
         ++after;
         while (isspace((unsigned char)*after)) ++after;
         return after;
      }
      p = end;
   }
   return NULL;
}

int lunaria_cdp_json_string(const char *json, const char *key,
                            char *out, size_t capacity)
{
   const char *p = json_field(json, key);
   if (!p || !out || !capacity || *p != '"') return -1;
   const char *end = json_string_end(p);
   if (!end) return -1;
   size_t n = 0;
   for (++p; p < end - 1; ++p) {
      unsigned char c = (unsigned char)*p;
      if (c == '\\') {
         c = (unsigned char)*++p;
         switch (c) {
         case 'n': c = '\n'; break;
         case 'r': c = '\r'; break;
         case 't': c = '\t'; break;
         case 'b': c = '\b'; break;
         case 'f': c = '\f'; break;
         case '"': case '\\': case '/': break;
         default: return -1; /* CDP IDs are ASCII; no \u escaping needed. */
         }
      }
      if (n + 1 >= capacity) return -1;
      out[n++] = (char)c;
   }
   out[n] = 0;
   return 0;
}

/* End of one JSON value (any type) starting at p. */
static const char *json_value_end(const char *p)
{
   if (*p == '"') return json_string_end(p);
   if (*p == '{' || *p == '[') {
      int depth = 0;
      while (*p) {
         if (*p == '"') { p = json_string_end(p); if (!p) return NULL; continue; }
         if (*p == '{' || *p == '[') ++depth;
         else if (*p == '}' || *p == ']') { if (--depth == 0) return p + 1; }
         ++p;
      }
      return NULL;
   }
   while (*p && *p != ',' && *p != '}' && *p != ']' && !isspace((unsigned char)*p)) ++p;
   return p;
}

char *lunaria_cdp_json_raw(const char *json, const char *key)
{
   const char *p = json_field(json, key);
   const char *end = p ? json_value_end(p) : NULL;
   if (!end || end == p) return NULL;
   char *out = malloc((size_t)(end - p) + 1);
   if (!out) return NULL;
   memcpy(out, p, (size_t)(end - p));
   out[end - p] = 0;
   return out;
}

static void utf8_append(char **w, unsigned cp)
{
   char *p = *w;
   if (cp < 0x80) *p++ = (char)cp;
   else if (cp < 0x800) { *p++ = (char)(0xc0 | cp >> 6); *p++ = (char)(0x80 | (cp & 63)); }
   else if (cp < 0x10000) { *p++ = (char)(0xe0 | cp >> 12); *p++ = (char)(0x80 | ((cp >> 6) & 63)); *p++ = (char)(0x80 | (cp & 63)); }
   else { *p++ = (char)(0xf0 | cp >> 18); *p++ = (char)(0x80 | ((cp >> 12) & 63));
          *p++ = (char)(0x80 | ((cp >> 6) & 63)); *p++ = (char)(0x80 | (cp & 63)); }
   *w = p;
}

char *lunaria_cdp_json_string_at(const char *p)
{
   const char *end = p && *p == '"' ? json_string_end(p) : NULL;
   if (!end) return NULL;
   char *out = malloc((size_t)(end - p) + 1), *w = out;
   if (!out) return NULL;
   for (++p; p < end - 1; ++p) {
      if (*p != '\\') { *w++ = *p; continue; }
      switch (*++p) {
      case 'n': *w++ = '\n'; break;
      case 'r': *w++ = '\r'; break;
      case 't': *w++ = '\t'; break;
      case 'b': *w++ = '\b'; break;
      case 'f': *w++ = '\f'; break;
      case 'u': {
         unsigned cp = (unsigned)strtoul((char[5]){ p[1], p[2], p[3], p[4], 0 }, NULL, 16);
         p += 4;
         if (cp >= 0xd800 && cp < 0xdc00 && p[1] == '\\' && p[2] == 'u') {
            unsigned lo = (unsigned)strtoul((char[5]){ p[3], p[4], p[5], p[6], 0 }, NULL, 16);
            if (lo >= 0xdc00 && lo < 0xe000) { cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00); p += 6; }
         }
         utf8_append(&w, cp);
         break;
      }
      default: *w++ = *p;
      }
   }
   *w = 0;
   return out;
}

char *lunaria_cdp_json_string_dup(const char *json, const char *key)
{
   return lunaria_cdp_json_string_at(json_field(json, key));
}

int lunaria_cdp_json_integer(const char *json, const char *key, long *out)
{
   const char *p = json_field(json, key);
   if (!p || !out) return -1;
   char *end;
   long value = strtol(p, &end, 10);
   if (end == p || (*end && *end != ',' && *end != '}' &&
                    !isspace((unsigned char)*end))) return -1;
   *out = value;
   return 0;
}

char *lunaria_cdp_json_quote(const char *value)
{
   if (!value) return NULL;
   size_t len = strlen(value);
   if (len > (SIZE_MAX - 3) / 6) return NULL;
   char *out = malloc(len * 6 + 3);
   if (!out) return NULL;
   char *p = out;
   *p++ = '"';
   static const char hex[] = "0123456789abcdef";
   for (const unsigned char *s = (const unsigned char *)value; *s; ++s) {
      unsigned char c = *s;
      if (c == '"' || c == '\\') { *p++ = '\\'; *p++ = (char)c; }
      else if (c < 0x20) {
         *p++ = '\\'; *p++ = 'u'; *p++ = '0'; *p++ = '0';
         *p++ = hex[c >> 4]; *p++ = hex[c & 15];
      } else *p++ = (char)c;
   }
   *p++ = '"'; *p = 0;
   return out;
}

static int base64_digit(unsigned char c)
{
   if (c >= 'A' && c <= 'Z') return c - 'A';
   if (c >= 'a' && c <= 'z') return c - 'a' + 26;
   if (c >= '0' && c <= '9') return c - '0' + 52;
   if (c == '+') return 62;
   if (c == '/') return 63;
   return -1;
}

unsigned char *lunaria_cdp_json_image(const char *json, size_t *length)
{
   if (length) *length = 0;
   const char *p = json_field(json, "data");
   if (!p || !length || *p++ != '"') return NULL;
   const char *end = strchr(p, '"');
   if (!end) return NULL;
   const size_t encoded = (size_t)(end - p);
   if (encoded < 4 || encoded % 4 || encoded / 4 > SIZE_MAX / 3)
      return NULL;
   const size_t capacity = encoded / 4 * 3;
   unsigned char *out = malloc(capacity);
   if (!out) return NULL;
   size_t used = 0;
   for (const char *s = p; s < end; s += 4) {
      int a = base64_digit((unsigned char)s[0]);
      int b = base64_digit((unsigned char)s[1]);
      int c = s[2] == '=' ? 0 : base64_digit((unsigned char)s[2]);
      int d = s[3] == '=' ? 0 : base64_digit((unsigned char)s[3]);
      if (a < 0 || b < 0 || c < 0 || d < 0 ||
          (s[2] == '=' && s[3] != '=') ||
          ((s[2] == '=' || s[3] == '=') && s + 4 != end)) {
         free(out);
         return NULL;
      }
      out[used++] = (unsigned char)((a << 2) | (b >> 4));
      if (s[2] != '=') out[used++] = (unsigned char)((b << 4) | (c >> 2));
      if (s[3] != '=') out[used++] = (unsigned char)((c << 6) | d);
   }
   *length = used;
   return out;
}

static char *wait_reply(struct lunaria_cdp_page *page, unsigned id)
{
   for (;;) {
      char *message = lunaria_cdp_receive(page->transport);
      if (!message) return NULL;
      long found;
      if (lunaria_cdp_json_integer(message, "id", &found) == 0 &&
          found == (long)id) return message;
      free(message); /* Initial about:blank events are superseded by navigate. */
   }
}

int lunaria_cdp_page_begin(struct lunaria_cdp_page *page,
                           struct lunaria_cdp *transport)
{
   if (!page || !transport) return -1;
   memset(page, 0, sizeof *page);
   page->transport = transport;
   page->next_id = 1;
   page->setup_id = page->next_id++;
   page->stage = 1;
   return lunaria_cdp_send(transport,
      "{\"id\":1,\"method\":\"Target.createTarget\","
      "\"params\":{\"url\":\"about:blank\"}}") == 0 ? 0 : -1;
}

int lunaria_cdp_page_poll(struct lunaria_cdp_page *page, char **message)
{
   if (message) *message = NULL;
   if (!page || !message || !page->transport || page->stage < 1) return -1;
   for (int i = 0; i < 32; ++i) {
      int read_status = -1;
      char *incoming = lunaria_cdp_try_receive(page->transport, &read_status);
      if (!incoming) return read_status < 0 ? -1 : 0;
      if (page->stage == 4) { *message = incoming; return 1; }
      long id = -1;
      int matching = lunaria_cdp_json_integer(incoming, "id", &id) == 0 &&
                     id == (long)page->setup_id;
      int failed = matching && json_field(incoming, "error");
      if (matching && !failed && page->stage == 1) {
         if (lunaria_cdp_json_string(incoming, "targetId", page->target_id,
                                      sizeof page->target_id) != 0) failed = 1;
         else {
            char command[512];
            page->setup_id = page->next_id++;
            snprintf(command, sizeof command,
               "{\"id\":%u,\"method\":\"Target.attachToTarget\","
               "\"params\":{\"targetId\":\"%s\",\"flatten\":true}}",
               page->setup_id, page->target_id);
            failed = lunaria_cdp_send(page->transport, command) != 0;
            page->stage = 2;
         }
      } else if (matching && !failed && page->stage == 2) {
         if (lunaria_cdp_json_string(incoming, "sessionId", page->session_id,
                                      sizeof page->session_id) != 0) failed = 1;
         else {
            char command[320];
            page->setup_id = page->next_id++;
            snprintf(command, sizeof command,
               "{\"id\":%u,\"sessionId\":\"%s\","
               "\"method\":\"Page.enable\"}",
               page->setup_id, page->session_id);
            failed = lunaria_cdp_send(page->transport, command) != 0;
            page->stage = 3;
         }
      } else if (matching && !failed && page->stage == 3) {
         page->stage = 4;
         free(incoming);
         return 2;
      }
      free(incoming);
      if (failed) return -1;
   }
   return 0;
}

int lunaria_cdp_page_open(struct lunaria_cdp_page *page,
                          struct lunaria_cdp *transport)
{
   if (!page || !transport) return -1;
   memset(page, 0, sizeof *page);
   page->transport = transport;
   page->next_id = 1;
   const unsigned create_id = page->next_id++;
   if (lunaria_cdp_send(transport,
      "{\"id\":1,\"method\":\"Target.createTarget\","
      "\"params\":{\"url\":\"about:blank\"}}") != 0) return -1;
   char *reply = wait_reply(page, create_id);
   char target[128];
   int ok = reply && lunaria_cdp_json_string(reply, "targetId", target,
                                              sizeof target) == 0;
   free(reply);
   if (!ok) return -1;

   char command[512];
   const unsigned attach_id = page->next_id++;
   snprintf(command, sizeof command,
      "{\"id\":%u,\"method\":\"Target.attachToTarget\","
      "\"params\":{\"targetId\":\"%s\",\"flatten\":true}}",
      attach_id, target);
   if (lunaria_cdp_send(transport, command) != 0) return -1;
   reply = wait_reply(page, attach_id);
   ok = reply && lunaria_cdp_json_string(reply, "sessionId",
                                          page->session_id,
                                          sizeof page->session_id) == 0;
   free(reply);
   if (!ok) return -1;

   const unsigned enable_id = page->next_id++;
   snprintf(command, sizeof command,
      "{\"id\":%u,\"sessionId\":\"%s\",\"method\":\"Page.enable\"}",
      enable_id, page->session_id);
   if (lunaria_cdp_send(transport, command) != 0) return -1;
   reply = wait_reply(page, enable_id);
   ok = reply && !json_field(reply, "error");
   free(reply);
   return ok ? 0 : -1;
}

int lunaria_cdp_page_navigate(struct lunaria_cdp_page *page, const char *url)
{
   if (!page || !page->transport || !page->session_id[0]) return -1;
   char *quoted = lunaria_cdp_json_quote(url);
   if (!quoted) return -1;
   size_t length = strlen(quoted) + strlen(page->session_id) + 128;
   char *command = malloc(length);
   if (!command) { free(quoted); return -1; }
   snprintf(command, length,
      "{\"id\":%u,\"sessionId\":\"%s\",\"method\":\"Page.navigate\","
      "\"params\":{\"url\":%s}}",
      page->next_id++, page->session_id, quoted);
   int status = lunaria_cdp_send(page->transport, command);
   free(command);
   free(quoted);
   return status;
}

int lunaria_cdp_page_capture(struct lunaria_cdp_page *page,
                             unsigned *request_id)
{
   if (!page || !page->transport || !page->session_id[0]) return -1;
   char command[320];
   const unsigned id = page->next_id++;
   snprintf(command, sizeof command,
      "{\"id\":%u,\"sessionId\":\"%s\","
      "\"method\":\"Page.captureScreenshot\","
      "\"params\":{\"format\":\"png\"}}",
      id, page->session_id);
   if (lunaria_cdp_send(page->transport, command) != 0) return -1;
   if (request_id) *request_id = id;
   return 0;
}

int lunaria_cdp_page_mouse(struct lunaria_cdp_page *page,
                           int x, int y, int pressed,
                           unsigned *request_id)
{
   if (!page || !page->transport || !page->session_id[0]) return -1;
   char command[512];
   const unsigned id = page->next_id++;
   snprintf(command, sizeof command,
      "{\"id\":%u,\"sessionId\":\"%s\","
      "\"method\":\"Input.dispatchMouseEvent\","
      "\"params\":{\"type\":\"mouse%s\",\"x\":%d,\"y\":%d,"
      "\"button\":\"left\",\"clickCount\":1}}",
      id, page->session_id, pressed ? "Pressed" : "Released", x, y);
   if (lunaria_cdp_send(page->transport, command) != 0) return -1;
   if (request_id) *request_id = id;
   return 0;
}

int lunaria_cdp_page_touch(struct lunaria_cdp_page *page,
                           int x, int y, int action)
{
   if (!page || !page->transport || !page->session_id[0] ||
       (action != 1 && action != -1 && action != 0)) return -1;
   char command[448];
   const unsigned id = page->next_id++;
   const char *type = action == 1 ? "touchStart" :
                      action == -1 ? "touchMove" : "touchEnd";
   if (action == 0)
      snprintf(command, sizeof command,
         "{\"id\":%u,\"sessionId\":\"%s\","
         "\"method\":\"Input.dispatchTouchEvent\","
         "\"params\":{\"type\":\"%s\",\"touchPoints\":[]}}",
         id, page->session_id, type);
   else
      snprintf(command, sizeof command,
         "{\"id\":%u,\"sessionId\":\"%s\","
         "\"method\":\"Input.dispatchTouchEvent\","
         "\"params\":{\"type\":\"%s\","
         "\"touchPoints\":[{\"x\":%d,\"y\":%d}]}}",
         id, page->session_id, type, x, y);
   return lunaria_cdp_send(page->transport, command);
}

int lunaria_cdp_page_viewport(struct lunaria_cdp_page *page,
                              int width, int height)
{
   return lunaria_cdp_page_viewport_scaled(page, width, height, 1.0);
}

int lunaria_cdp_page_viewport_scaled(struct lunaria_cdp_page *page,
                                     int width, int height, double scale)
{
   if (!page || !page->transport || !page->session_id[0] ||
       width <= 0 || height <= 0 || width > 8192 || height > 8192) return -1;
   char command[384];
   const unsigned id = page->next_id++;
   snprintf(command, sizeof command,
      "{\"id\":%u,\"sessionId\":\"%s\","
      "\"method\":\"Emulation.setDeviceMetricsOverride\","
      "\"params\":{\"width\":%d,\"height\":%d,"
      "\"deviceScaleFactor\":%.4g,\"mobile\":true}}",
      id, page->session_id, width, height, scale > 0.0 ? scale : 1.0);
   if (lunaria_cdp_send(page->transport, command) != 0) return -1;
   snprintf(command, sizeof command,
      "{\"id\":%u,\"sessionId\":\"%s\","
      "\"method\":\"Emulation.setTouchEmulationEnabled\","
      "\"params\":{\"enabled\":true,\"maxTouchPoints\":1}}",
      page->next_id++, page->session_id);
   return lunaria_cdp_send(page->transport, command);
}

static int send_quoted(struct lunaria_cdp_page *page, const char *method,
                        const char *key, const char *value,
                        unsigned *request_id)
{
   if (!page || !page->transport || !page->session_id[0]) return -1;
   char *quoted = lunaria_cdp_json_quote(value);
   if (!quoted) return -1;
   size_t length = strlen(quoted) + strlen(page->session_id) + 160;
   char *command = malloc(length);
   if (!command) { free(quoted); return -1; }
   const unsigned id = page->next_id++;
   snprintf(command, length,
      "{\"id\":%u,\"sessionId\":\"%s\",\"method\":\"%s\","
      "\"params\":{\"%s\":%s}}",
      id, page->session_id, method, key, quoted);
   int status = lunaria_cdp_send(page->transport, command);
   free(command);
   free(quoted);
   if (status == 0 && request_id) *request_id = id;
   return status;
}

int lunaria_cdp_page_insert_text(struct lunaria_cdp_page *page,
                                 const char *value, unsigned *request_id)
{
   return send_quoted(page, "Input.insertText", "text", value, request_id);
}

int lunaria_cdp_page_key(struct lunaria_cdp_page *page,
                         const char *key, int virtual_code, int pressed)
{
   if (!page || !page->transport || !page->session_id[0] ||
       !key || strlen(key) > 32 || virtual_code < 0) return -1;
   char command[384];
   snprintf(command, sizeof command,
      "{\"id\":%u,\"sessionId\":\"%s\","
      "\"method\":\"Input.dispatchKeyEvent\","
      "\"params\":{\"type\":\"key%s\",\"key\":\"%s\","
      "\"windowsVirtualKeyCode\":%d}}",
      page->next_id++, page->session_id,
      pressed ? "Down" : "Up", key, virtual_code);
   return lunaria_cdp_send(page->transport, command);
}

int lunaria_cdp_page_command(struct lunaria_cdp_page *page, const char *method,
                             const char *params_json, unsigned *request_id)
{
   if (!page || !page->transport || !page->session_id[0] || !method) return -1;
   if (!params_json) params_json = "{}";
   size_t length = strlen(params_json) + strlen(method) + strlen(page->session_id) + 96;
   char *command = malloc(length);
   if (!command) return -1;
   const unsigned id = page->next_id++;
   snprintf(command, length,
      "{\"id\":%u,\"sessionId\":\"%s\",\"method\":\"%s\",\"params\":%s}",
      id, page->session_id, method, params_json);
   int status = lunaria_cdp_send(page->transport, command);
   free(command);
   if (status == 0 && request_id) *request_id = id;
   return status;
}

int lunaria_cdp_page_eval(struct lunaria_cdp_page *page,
                          const char *expression, unsigned *request_id)
{
   char *quoted = lunaria_cdp_json_quote(expression);
   if (!quoted) return -1;
   size_t length = strlen(quoted) + 64;
   char *params = malloc(length);
   if (!params) { free(quoted); return -1; }
   /* By value: WebView.evaluateJavascript answers with the value's JSON. */
   snprintf(params, length, "{\"expression\":%s,\"returnByValue\":true}", quoted);
   int status = lunaria_cdp_page_command(page, "Runtime.evaluate", params, request_id);
   free(params);
   free(quoted);
   return status;
}

char *lunaria_cdp_page_receive(struct lunaria_cdp_page *page)
{
   return page && page->transport ? lunaria_cdp_receive(page->transport) : NULL;
}
