/* Copyright Joyent, Inc. and other Node contributors. All rights reserved.
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

/* This file contains both the uv__async internal infrastructure and the
 * user-facing uv_async_t functions.
 */

#include "uv.h"
#include "internal.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>  /* snprintf() */
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sched.h>  /* sched_yield() */

#ifdef __linux__
#include <sys/eventfd.h>
#endif

static void uv__async_send(uv_loop_t* loop);
static int uv__async_start(uv_loop_t* loop);
#if !defined(UV_PREFER_WAIT)
static void uv__cpu_relax(void);
#endif


int uv_async_init(uv_loop_t* loop, uv_async_t* handle, uv_async_cb async_cb) {
  int err;

  err = uv__async_start(loop);
  if (err)
    return err;

  uv__handle_init(loop, (uv_handle_t*)handle, UV_ASYNC);
  handle->async_cb = async_cb;
  handle->pending = 0;
#if defined(UV_PREFER_WAIT)
  if (uv_cond_init(&handle->not_busy))
    abort();
  if (uv_mutex_init(&handle->mutex))
    abort();
#endif

  QUEUE_INSERT_TAIL(&loop->async_handles, &handle->queue);
  uv__handle_start(handle);

  return 0;
}


int uv_async_send(uv_async_t* handle) {
  _Atomic int* pending;
  int expected;

  pending = (_Atomic int*) &handle->pending;

  /* Do a cheap read first. */
  if (atomic_load_explicit(pending, memory_order_relaxed) != 0)
    return 0;

  /* Tell the other thread we're busy with the handle. */
  expected = 0;
  if (!atomic_compare_exchange_strong(pending, &expected, 1))
    return 0;

  /* Wake up the other thread's event loop. */
  uv__async_send(handle->loop);

  /* Tell the other thread we're done. */
  expected = 1;
#if defined(UV_PREFER_WAIT)
  uv_mutex_lock(&handle->mutex);
#endif
  if (!atomic_compare_exchange_strong(pending, &expected, 2))
    abort();
#if defined(UV_PREFER_WAIT)
  if (handle->pending != 1) {
    uv_cond_broadcast(&handle->not_busy);
    uv_mutex_unlock(&handle->mutex);
  }
#endif

  return 0;
}

/* Only call uv__async_wait and uv__async_spin from the event loop thread. */
#if defined(UV_PREFER_WAIT)
/* For platforms where waiting is better than spin & yield */
static int uv__async_wait(uv_async_t* handle) {
  _Atomic int* pending;
  int expected;

  pending = (_Atomic int*) &handle->pending;
  uv_mutex_lock(&handle->mutex);
  for (;;) {
    expected = 2;
    atomic_compare_exchange_strong(pending, &expected, 0);
    if (expected != 1)
      break;
    uv_cond_wait(&handle->not_busy, &handle->mutex);
  }
  uv_mutex_unlock(&handle->mutex);
  return expected;
}
#else
/*  For platforms where spin & yield is better than waiting */
static int uv__async_spin(uv_async_t* handle) {
  _Atomic int* pending;
  int expected;
  int i;

  pending = (_Atomic int*) &handle->pending;

  for (;;) {
    /* 997 is not completely chosen at random. It's a prime number, acyclical
     * by nature, and should therefore hopefully dampen sympathetic resonance.
     */
    for (i = 0; i < 997; i++) {
      /* rc=0 -- handle is not pending.
       * rc=1 -- handle is pending, other thread is still working with it.
       * rc=2 -- handle is pending, other thread is done.
       */
      expected = 2;
      atomic_compare_exchange_strong(pending, &expected, 0);

      if (expected != 1)
        return expected;

      /* Other thread is busy with this handle, spin until it's done. */
      uv__cpu_relax();
    }

    /* Yield the CPU. We may have preempted the other thread while it's
     * inside the critical section and if it's running on the same CPU
     * as us, we'll just burn CPU cycles until the end of our time slice.
     */
    sched_yield();
  }
}
#endif

void uv__async_close(uv_async_t* handle) {
#if defined(UV_PREFER_WAIT)
  uv__async_wait(handle);
#else
  uv__async_spin(handle);
#endif
  QUEUE_REMOVE(&handle->queue);
  uv__handle_stop(handle);
}


static void uv__async_io(uv_loop_t* loop, uv__io_t* w, unsigned int events) {
  char buf[1024];
  ssize_t r;
  QUEUE queue;
  QUEUE* q;
  uv_async_t* h;

  assert(w == &loop->async_io_watcher);

  for (;;) {
    r = read(w->fd, buf, sizeof(buf));

    if (r == sizeof(buf))
      continue;

    if (r != -1)
      break;

    if (errno == EAGAIN || errno == EWOULDBLOCK)
      break;

    if (errno == EINTR)
      continue;

    abort();
  }

  QUEUE_MOVE(&loop->async_handles, &queue);
  while (!QUEUE_EMPTY(&queue)) {
    q = QUEUE_HEAD(&queue);
    h = QUEUE_DATA(q, uv_async_t, queue);

    QUEUE_REMOVE(q);
    QUEUE_INSERT_TAIL(&loop->async_handles, q);

#if defined(UV_PREFER_WAIT)
    if (0 == uv__async_wait(h))
#else
    if (0 == uv__async_spin(h))
#endif
      continue;  /* Not pending. */

    if (h->async_cb == NULL)
      continue;

    h->async_cb(h);
  }
}


static void uv__async_send(uv_loop_t* loop) {
  const void* buf;
  ssize_t len;
  int fd;
  int r;

  buf = "";
  len = 1;
  fd = loop->async_wfd;

#if defined(__linux__)
  if (fd == -1) {
    static const uint64_t val = 1;
    buf = &val;
    len = sizeof(val);
    fd = loop->async_io_watcher.fd;  /* eventfd */
  }
#endif

  do
    r = write(fd, buf, len);
  while (r == -1 && errno == EINTR);

  if (r == len)
    return;

  if (r == -1)
    if (errno == EAGAIN || errno == EWOULDBLOCK)
      return;

  abort();
}


static int uv__async_start(uv_loop_t* loop) {
  int pipefd[2];
  int err;

  if (loop->async_io_watcher.fd != -1)
    return 0;

#ifdef __linux__
  err = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  if (err < 0)
    return UV__ERR(errno);

  pipefd[0] = err;
  pipefd[1] = -1;
#else
  err = uv__make_pipe(pipefd, UV_NONBLOCK_PIPE);
  if (err < 0)
    return err;
#endif

  uv__io_init(&loop->async_io_watcher, uv__async_io, pipefd[0]);
  uv__io_start(loop, &loop->async_io_watcher, POLLIN);
  loop->async_wfd = pipefd[1];

  return 0;
}


int uv__async_fork(uv_loop_t* loop) {
  if (loop->async_io_watcher.fd == -1) /* never started */
    return 0;

  uv__async_stop(loop);

  return uv__async_start(loop);
}


void uv__async_stop(uv_loop_t* loop) {
  if (loop->async_io_watcher.fd == -1)
    return;

  if (loop->async_wfd != -1) {
    if (loop->async_wfd != loop->async_io_watcher.fd)
      uv__close(loop->async_wfd);
    loop->async_wfd = -1;
  }

  uv__io_stop(loop, &loop->async_io_watcher, POLLIN);
  uv__close(loop->async_io_watcher.fd);
  loop->async_io_watcher.fd = -1;
}

#if !defined(UV_PREFER_WAIT)
static void uv__cpu_relax(void) {
#if defined(__i386__) || defined(__x86_64__)
  __asm__ __volatile__ ("rep; nop" ::: "memory");  /* a.k.a. PAUSE */
#elif (defined(__arm__) && __ARM_ARCH >= 7) || defined(__aarch64__)
  __asm__ __volatile__ ("yield" ::: "memory");
#elif (defined(__ppc__) || defined(__ppc64__)) && defined(__APPLE__)
  __asm volatile ("" : : : "memory");
#elif !defined(__APPLE__) && (defined(__powerpc64__) || defined(__ppc64__) || defined(__PPC64__))
  __asm__ __volatile__ ("or 1,1,1; or 2,2,2" ::: "memory");
#endif
}
#endif
