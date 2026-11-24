/* SPDX-License-Identifier: MPL-2.0 */
/* Socket and eventfd readiness in the Android descriptor table. */

enum {
  WIN_EPOLL_IN = 0x001, WIN_EPOLL_OUT = 0x004,
  WIN_EPOLL_ERR = 0x008, WIN_EPOLL_HUP = 0x010,
  WIN_EPOLL_RDNORM = 0x040, WIN_EPOLL_WRNORM = 0x100,
  WIN_EPOLL_RDHUP = 0x2000,
  WIN_EPOLL_ONESHOT = 0x40000000u,
  WIN_EPOLL_ADD = 1, WIN_EPOLL_DEL = 2, WIN_EPOLL_MOD = 3
};

#define WIN_EPOLL_READ (WIN_EPOLL_IN | WIN_EPOLL_RDNORM)
#define WIN_EPOLL_WRITE (WIN_EPOLL_OUT | WIN_EPOLL_WRNORM)
#define WIN_EPOLL_EDGE UINT32_C(0x80000000)
#define WIN_EPOLL_MASK (WIN_EPOLL_READ | WIN_EPOLL_WRITE | WIN_EPOLL_ERR | \
                        WIN_EPOLL_HUP | WIN_EPOLL_RDHUP | WIN_EPOLL_ONESHOT | WIN_EPOLL_EDGE)
#define WIN_EVENT_MAX_COUNT (UINT64_MAX - UINT64_C(1))

struct win_epoll_entry {
  int fd;
  struct socket_state *description;
  luna_os_poll_event interest;
  uint64_t read_epoch, write_epoch;
  uint32_t observed, pending;
  bool disabled;
};

struct win_epoll {
  struct win_epoll_entry *entries;
  WSAPOLLFD *native;
  size_t count, capacity, cursor;
};

static void win_epoll_free(struct win_epoll *poller)
{
  if (!poller) return;
  free(poller->entries);
  free(poller->native);
  free(poller);
}

static void win_epoll_remove_at(struct win_epoll *poller, size_t at)
{
  --poller->count;
  memmove(&poller->entries[at], &poller->entries[at + 1],
          (poller->count - at) * sizeof *poller->entries);
  if (poller->cursor > at) --poller->cursor;
  if (poller->cursor >= poller->count) poller->cursor = 0;
}

/* Watches follow an open file description across dup and disappear on last close. */
static void win_epoll_forget_locked(struct socket_state *description)
{
  for (struct socket_fd *fd = sockets; fd; fd = fd->next) {
    struct win_epoll *poller = fd->state->poller;
    if (!poller) continue;
    for (size_t i = poller->count; i > 0; --i)
      if (poller->entries[i - 1].description == description) win_epoll_remove_at(poller, i - 1);
  }
}

static int win_event_transfer(int fd, uint64_t *count, bool writing)
{
  const uint64_t amount = writing ? *count : 0;
  if (writing && amount == UINT64_MAX) {
    errno = EINVAL;
    return -1;
  }
  int error = 0;
  uint64_t identity = 0;
  AcquireSRWLockExclusive(&socket_lock);
  for (;;) {
    struct socket_fd *descriptor = socket_find_locked(fd);
    if (!descriptor || (identity && identity != descriptor->state->identity)) {
      error = EBADF;
      break;
    }
    struct socket_state *event = descriptor->state;
    if (event->kind != WIN_FD_EVENT) {
      error = EINVAL;
      break;
    }
    identity = event->identity;
    if (writing && amount <= WIN_EVENT_MAX_COUNT - event->event_count) {
      event->event_count += amount;
      if (event->event_count == WIN_EVENT_MAX_COUNT) ++event->write_epoch;
      WakeAllConditionVariable(&descriptor_changed);
      break;
    }
    if (!writing && event->event_count) {
      if (count) *count = event->event_count;
      event->event_count = 0;
      ++event->read_epoch;
      WakeAllConditionVariable(&descriptor_changed);
      break;
    }
    if (event->nonblock) {
      error = EAGAIN;
      break;
    }
    if (!SleepConditionVariableSRW(&descriptor_changed, &socket_lock, INFINITE, 0)) {
      error = EIO;
      break;
    }
  }
  ReleaseSRWLockExclusive(&socket_lock);
  if (error) errno = error;
  return error ? -1 : 0;
}

int luna_os_event_open(unsigned initval, int nonblock)
{
  return wrap_descriptor(INVALID_SOCKET, WIN_FD_EVENT, nonblock, initval, NULL);
}

int luna_os_event_signal(int fd, uint64_t count)
{
  return win_event_transfer(fd, &count, true);
}

int luna_os_event_drain(int fd, uint64_t *out)
{
  return win_event_transfer(fd, out, false);
}

static ptrdiff_t win_event_fd_io(int fd, void *buffer, size_t length, bool writing)
{
  uint64_t count = 0;
  if (length < sizeof count || (writing && length != sizeof count)) {
    errno = EINVAL;
    return -1;
  }
  if (!buffer) {
    errno = EFAULT;
    return -1;
  }
  if (writing) memcpy(&count, buffer, sizeof count);
  if (win_event_transfer(fd, &count, writing)) return -1;
  if (!writing) memcpy(buffer, &count, sizeof count);
  return sizeof count;
}

static uint32_t win_event_ready(const struct socket_state *event)
{
  uint32_t bits = event->event_count ? WIN_EPOLL_READ : 0;
  if (event->event_count < WIN_EVENT_MAX_COUNT) bits |= WIN_EPOLL_WRITE;
  return bits;
}

/* Polling snapshots readiness. Record transitions observed by guest I/O too,
 * so draining a socket and receiving again between snapshots cannot hide
 * the next edge. The descriptor and native socket must still match. */
static void win_socket_epoch(int fd, SOCKET socket, bool writing)
{
  AcquireSRWLockExclusive(&socket_lock);
  struct socket_fd *entry = socket_find_locked(fd);
  if (entry && entry->socket == socket) {
    if (writing) ++entry->state->write_epoch;
    else ++entry->state->read_epoch;
  }
  ReleaseSRWLockExclusive(&socket_lock);
}

static void win_socket_io_completed(int fd, SOCKET socket, bool writing, bool peek,
                                    size_t requested, size_t moved, int error)
{
  if (peek || !requested) return;
  if (error == WSAEWOULDBLOCK || (!error && (writing ? moved < requested : moved > 0)))
    win_socket_epoch(fd, socket, writing);
}

int luna_os_poll_create(int cloexec)
{
  struct win_epoll *poller = calloc(1, sizeof *poller);
  if (!poller) {
    errno = ENOMEM;
    return -1;
  }
  int fd = wrap_descriptor(INVALID_SOCKET, WIN_FD_EPOLL, 0, 0, poller);
  if (fd < 0) {
    win_epoll_free(poller);
    return -1;
  }
  if (!cloexec && luna_fd_cloexec(fd, 0)) {
    int error = errno;
    luna_fd_close(fd);
    errno = error;
    return -1;
  }
  return fd;
}

static int win_epoll_grow(struct win_epoll *poller)
{
  if (poller->count < poller->capacity) return 0;
  const size_t capacity = poller->capacity ? poller->capacity * 2 : 16;
  if (capacity > INT_MAX || capacity > SIZE_MAX / sizeof *poller->entries) return ENOMEM;
  struct win_epoll_entry *entries = realloc(poller->entries, capacity * sizeof *entries);
  if (!entries) return ENOMEM;
  poller->entries = entries;
  WSAPOLLFD *native = realloc(poller->native, capacity * sizeof *native);
  if (!native) return ENOMEM;
  poller->native = native;
  poller->capacity = capacity;
  return 0;
}

int luna_os_poll_ctl(int pollfd, int op, int fd, const luna_os_poll_event *event)
{
  if (op != WIN_EPOLL_ADD && op != WIN_EPOLL_MOD && op != WIN_EPOLL_DEL) {
    errno = EINVAL;
    return -1;
  }
  if (op != WIN_EPOLL_DEL && !event) {
    errno = EFAULT;
    return -1;
  }
  if (op != WIN_EPOLL_DEL && (event->events & ~(uint32_t)WIN_EPOLL_MASK)) {
    errno = EOPNOTSUPP;
    return -1;
  }
  int error = 0;
  AcquireSRWLockExclusive(&socket_lock);
  struct socket_fd *owner = socket_find_locked(pollfd);
  struct socket_fd *watched = socket_find_locked(fd);
  if (!owner) {
    error = file_descriptor_handle(pollfd) == -1 ? EBADF : EINVAL;
  } else if (!owner->state->poller) {
    error = EINVAL;
  } else if (!watched) {
    error = file_descriptor_handle(fd) == -1 ? EBADF : EPERM;
  } else if (owner->state == watched->state) {
    error = EINVAL;
  } else if (watched->state->kind == WIN_FD_EPOLL) {
    error = EOPNOTSUPP;
  } else {
    struct win_epoll *poller = owner->state->poller;
    size_t i = 0;
    for (; i < poller->count; ++i)
      if (poller->entries[i].fd == fd && poller->entries[i].description == watched->state) break;
    if (op == WIN_EPOLL_ADD && i < poller->count) error = EEXIST;
    else if (op != WIN_EPOLL_ADD && i == poller->count) error = ENOENT;
    else if (op == WIN_EPOLL_DEL) win_epoll_remove_at(poller, i);
    else {
      if (op == WIN_EPOLL_ADD && !(error = win_epoll_grow(poller))) ++poller->count;
      if (!error)
        poller->entries[i] = (struct win_epoll_entry) {
          .fd = fd, .description = watched->state, .interest = *event,
        };
    }
  }
  if (!error) WakeAllConditionVariable(&descriptor_changed);
  ReleaseSRWLockExclusive(&socket_lock);
  if (error) errno = error;
  return error ? -1 : 0;
}

static uint32_t win_epoll_socket_ready(short native)
{
  uint32_t events = 0;
  if (native & POLLIN) events |= WIN_EPOLL_READ;
  if (native & POLLOUT) events |= WIN_EPOLL_WRITE;
  if (native & (POLLERR | POLLNVAL)) events |= WIN_EPOLL_ERR;
  if (native & POLLHUP) events |= WIN_EPOLL_HUP | WIN_EPOLL_RDHUP | WIN_EPOLL_READ;
  return events;
}

static int win_epoll_scan_locked(struct win_epoll *poller, luna_os_poll_event *events, int capacity)
{
  bool have_native = false;
  for (size_t i = 0; i < poller->count; ++i) {
    const struct win_epoll_entry *entry = &poller->entries[i];
    const uint32_t wanted = entry->interest.events;
    WSAPOLLFD *native = &poller->native[i];
    *native = (WSAPOLLFD){.fd = INVALID_SOCKET};
    if (entry->disabled || entry->description->kind != WIN_FD_SOCKET) continue;
    have_native = true;
    native->fd = entry->description->watch_socket;
    if (wanted & (WIN_EPOLL_READ | WIN_EPOLL_RDHUP)) native->events |= POLLIN;
    if (wanted & WIN_EPOLL_WRITE) native->events |= POLLOUT;
  }
  if (have_native && WSAPoll(poller->native, (ULONG)poller->count, 0) == SOCKET_ERROR)
    return failed();

  int found = 0;
  const size_t first = poller->cursor;
  size_t i = first;
  for (size_t visited = 0; visited < poller->count; ++visited, i = (i + 1) % poller->count) {
    struct win_epoll_entry *entry = &poller->entries[i];
    if (entry->disabled) continue;
    struct socket_state *watched = entry->description;
    uint32_t ready = watched->kind == WIN_FD_EVENT ? win_event_ready(watched) :
                      win_epoll_socket_ready(poller->native[i].revents);
    ready &= entry->interest.events | WIN_EPOLL_ERR | WIN_EPOLL_HUP;
    if (entry->interest.events & WIN_EPOLL_EDGE) {
      uint32_t changed = ready & ~entry->observed;
      if (watched->read_epoch != entry->read_epoch) changed |= ready & WIN_EPOLL_READ;
      if (watched->write_epoch != entry->write_epoch) changed |= ready & WIN_EPOLL_WRITE;
      entry->read_epoch = watched->read_epoch;
      entry->write_epoch = watched->write_epoch;
      entry->observed = ready;
      entry->pending = (entry->pending & ready) | changed;
      ready = entry->pending;
    }
    if (!ready || found == capacity) continue;
    events[found++] = (luna_os_poll_event){.events = ready, .data = entry->interest.data};
    entry->pending = 0;
    entry->disabled = !!(entry->interest.events & WIN_EPOLL_ONESHOT);
    poller->cursor = (i + 1) % poller->count;
  }
  return found;
}

int luna_os_poll_wait(int pollfd, luna_os_poll_event *events, int max_events, int timeout_ms)
{
  if (!events || max_events <= 0) {
    errno = EINVAL;
    return -1;
  }
  uint64_t identity = 0;
  const ULONGLONG started = GetTickCount64();
  AcquireSRWLockExclusive(&socket_lock);
  int ready;
  for (;;) {
    struct socket_fd *fd = socket_find_locked(pollfd);
    if (!fd || (identity && identity != fd->state->identity)) {
      errno = !identity && file_descriptor_handle(pollfd) != -1 ? EINVAL : EBADF;
      ready = -1;
      break;
    }
    if (!fd->state->poller) {
      errno = EINVAL;
      ready = -1;
      break;
    }
    identity = fd->state->identity;
    ready = win_epoll_scan_locked(fd->state->poller, events, max_events);
    if (ready || !timeout_ms) break;
    const ULONGLONG elapsed = GetTickCount64() - started;
    if (timeout_ms > 0 && elapsed >= (ULONGLONG)timeout_ms) break;
    /* Native sockets are sampled. Counter writes, ctl and close can wake
     * waiters immediately through the same lock. The ARM scheduler polls at 0. */
    DWORD delay = 2;
    if (timeout_ms > 0 && (ULONGLONG)timeout_ms - elapsed < delay)
      delay = (DWORD)((ULONGLONG)timeout_ms - elapsed);
    if (!SleepConditionVariableSRW(&descriptor_changed, &socket_lock, delay, 0) &&
        GetLastError() != ERROR_TIMEOUT) {
      errno = EIO;
      ready = -1;
      break;
    }
  }
  ReleaseSRWLockExclusive(&socket_lock);
  return ready;
}
