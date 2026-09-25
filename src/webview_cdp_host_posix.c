#define _POSIX_C_SOURCE 200809L
#include "webview_cdp.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

static ptrdiff_t host_read(void *context, void *dst, size_t size)
{
   struct lunaria_cdp_host *host = context;
   ssize_t n;
   do { n = read(host->input_fd, dst, size); } while (n < 0 && errno == EINTR);
   if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) &&
       host->nonblocking) return -2;
   return (ptrdiff_t)n;
}

int lunaria_cdp_host_set_nonblocking(struct lunaria_cdp_host *host)
{
   if (!host || host->input_fd < 0) return -1;
   int flags = fcntl(host->input_fd, F_GETFL);
   if (flags < 0 || fcntl(host->input_fd, F_SETFL, flags | O_NONBLOCK) < 0)
      return -1;
   host->nonblocking = 1;
   return 0;
}

static ptrdiff_t host_write(void *context, const void *src, size_t size)
{
   struct lunaria_cdp_host *host = context;
   ssize_t n;
   do { n = write(host->output_fd, src, size); } while (n < 0 && errno == EINTR);
   return (ptrdiff_t)n;
}

int lunaria_cdp_host_open(struct lunaria_cdp_host *host,
                          const char *browser_binary,
                          const char *profile_dir)
{
   if (!host || !browser_binary || !*browser_binary ||
       !profile_dir || !*profile_dir) return -1;
   memset(host, 0, sizeof *host);
   host->input_fd = host->output_fd = -1;
   if (mkdir(profile_dir, 0700) != 0 && errno != EEXIST) return -1;

   int to_browser[2] = { -1, -1 }, from_browser[2] = { -1, -1 };
   if (pipe(to_browser) != 0) return -1;
   if (pipe(from_browser) != 0) goto fail;
   /* posix_spawn file actions are ordered.  Move the source descriptors above
    * the protocol's fixed 3/4 first, so an adddup2 cannot overwrite another
    * pipe end when pipe() happened to allocate 3, 4, 5 and 6. */
   const int child_input = fcntl(to_browser[0], F_DUPFD, 5);
   const int child_output = fcntl(from_browser[1], F_DUPFD, 5);
   if (child_input < 0 || child_output < 0) {
      if (child_input >= 0) close(child_input);
      if (child_output >= 0) close(child_output);
      goto fail;
   }
   posix_spawn_file_actions_t actions;
   if (posix_spawn_file_actions_init(&actions) != 0) {
      close(child_input); close(child_output);
      goto fail;
   }
   int ok = posix_spawn_file_actions_addclose(&actions, to_browser[0]);
   if (!ok) ok = posix_spawn_file_actions_addclose(&actions, to_browser[1]);
   if (!ok) ok = posix_spawn_file_actions_addclose(&actions, from_browser[0]);
   if (!ok) ok = posix_spawn_file_actions_addclose(&actions, from_browser[1]);
   if (!ok) ok = posix_spawn_file_actions_adddup2(&actions, child_input, 3);
   if (!ok) ok = posix_spawn_file_actions_adddup2(&actions, child_output, 4);
   if (!ok) ok = posix_spawn_file_actions_addclose(&actions, child_input);
   if (!ok) ok = posix_spawn_file_actions_addclose(&actions, child_output);

   const size_t profile_len = strlen(profile_dir);
   char *profile_arg = malloc(profile_len + sizeof "--user-data-dir=");
   if (!profile_arg) ok = ENOMEM;
   if (!ok) sprintf(profile_arg, "--user-data-dir=%s", profile_dir);
   char *argv[] = {
      (char *)browser_binary, "--headless", "--no-first-run",
      "--no-default-browser-check", "--remote-debugging-pipe",
      profile_arg, "about:blank", NULL, NULL,
   };
   /* Chrome itself requires this when the emulator is run as root. */
   if (geteuid() == 0) argv[7] = "--no-sandbox";
   pid_t pid = -1;
   if (!ok) ok = posix_spawn(&pid, browser_binary, &actions, NULL,
                              argv, environ);
   free(profile_arg);
   posix_spawn_file_actions_destroy(&actions);
   close(child_input); close(child_output);
   if (ok) goto fail;

   close(to_browser[0]); close(from_browser[1]);
   host->input_fd = from_browser[0];
   host->output_fd = to_browser[1];
   host->process_id = (long)pid;
   host->cdp.context = host;
   host->cdp.read = host_read;
   host->cdp.write = host_write;
   return 0;
fail:
   if (to_browser[0] >= 0) close(to_browser[0]);
   if (to_browser[1] >= 0) close(to_browser[1]);
   if (from_browser[0] >= 0) close(from_browser[0]);
   if (from_browser[1] >= 0) close(from_browser[1]);
   return -1;
}

void lunaria_cdp_host_close(struct lunaria_cdp_host *host)
{
   if (!host) return;
   lunaria_cdp_close(&host->cdp);
   if (host->output_fd >= 0) close(host->output_fd);
   if (host->input_fd >= 0) close(host->input_fd);
   if (host->process_id > 0) {
      const pid_t pid = (pid_t)host->process_id;
      struct timespec delay = { .tv_sec = 0, .tv_nsec = 10000000 };
      for (int i = 0; i < 100; ++i) {
         if (waitpid(pid, NULL, WNOHANG) == pid) break;
         if (i == 99) {
            kill(pid, SIGTERM);
            (void)waitpid(pid, NULL, 0);
         } else {
            nanosleep(&delay, NULL);
         }
      }
   }
   host->input_fd = host->output_fd = -1;
   host->process_id = 0;
}
