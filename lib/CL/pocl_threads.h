/* pocl_threads.h - various helper macros & functions for multithreading

   Copyright (c) 2023 Jan Solanti / Tampere University

   Permission is hereby granted, free of charge, to any person obtaining a copy
   of this software and associated documentation files (the "Software"), to
   deal in the Software without restriction, including without limitation the
   rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
   sell copies of the Software, and to permit persons to whom the Software is
   furnished to do so, subject to the following conditions:

   The above copyright notice and this permission notice shall be included in
   all copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
   FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
   IN THE SOFTWARE.
*/

#ifndef POCL_THREADS_H
#define POCL_THREADS_H

#include "pocl_export.h"

/* To get adaptive mutex type */
#ifndef __USE_GNU
#define __USE_GNU
#endif

#include <pthread.h>
#include <signal.h>

typedef pthread_mutex_t pocl_lock_t;
typedef pthread_cond_t pocl_cond_t;
typedef pthread_t pocl_thread_t;
#define POCL_LOCK_INITIALIZER PTHREAD_MUTEX_INITIALIZER

#if defined(__GNUC__) || defined(__clang__)

/* These return the new value. */
/* See:
 * https://gcc.gnu.org/onlinedocs/gcc-4.7.4/gcc/_005f_005fatomic-Builtins.html
 */
#define POCL_ATOMIC_ADD(x, val) __atomic_add_fetch (&x, val, __ATOMIC_SEQ_CST);
#define POCL_ATOMIC_INC(x) __atomic_add_fetch (&x, 1, __ATOMIC_SEQ_CST)
#define POCL_ATOMIC_DEC(x) __atomic_sub_fetch (&x, 1, __ATOMIC_SEQ_CST)
#define POCL_ATOMIC_LOAD(x) __atomic_load_n (&x, __ATOMIC_SEQ_CST)
#define POCL_ATOMIC_STORE(x, val) __atomic_store_n (&x, val, __ATOMIC_SEQ_CST)
#define POCL_ATOMIC_CAS(ptr, oldval, newval)                                  \
  __sync_val_compare_and_swap (ptr, oldval, newval)

#elif defined(_WIN32)
#define POCL_ATOMIC_ADD(x, val) InterlockedAdd (&x, val);
#define POCL_ATOMIC_INC(x) InterlockedIncrement64 (&x)
#define POCL_ATOMIC_DEC(x) InterlockedDecrement64 (&x)
#define POCL_ATOMIC_LOAD(x) InterlockedOr64 (&x, 0)
#define POCL_ATOMIC_STORE(x, val) InterlockedExchange64 (&x, val)
#define POCL_ATOMIC_CAS(ptr, oldval, newval)                                  \
  InterlockedCompareExchange64 (ptr, newval, oldval)
#else
#error Need atomic_inc() builtin for this compiler
#endif

#ifdef __cplusplus
extern "C"
{
#endif
  /* Generic functionality for handling different types of
     OpenCL (host) objects. */

  POCL_EXPORT
  void pocl_abort_on_pthread_error (int status, unsigned line,
                                    const char *func);

#ifdef __cplusplus
}
#endif

/* Some pthread_*() calls may return '0' or a specific non-zero value on
 * success.
 */
#define PTHREAD_CHECK2(_status_ok, _code)                                     \
  do                                                                          \
    {                                                                         \
      int _pthread_status = (_code);                                          \
      if (_pthread_status != 0 && _pthread_status != (_status_ok))            \
        pocl_abort_on_pthread_error (_pthread_status, __LINE__,               \
                                     __FUNCTION__);                           \
    }                                                                         \
  while (0)

#define PTHREAD_CHECK(code) PTHREAD_CHECK2 (0, code)

/* Generic functionality for handling different types of
   OpenCL (host) objects. */

#define POCL_LOCK(__LOCK__) PTHREAD_CHECK (pthread_mutex_lock (&(__LOCK__)))
#define POCL_UNLOCK(__LOCK__)                                                 \
  PTHREAD_CHECK (pthread_mutex_unlock (&(__LOCK__)))
#define POCL_INIT_LOCK(__LOCK__)                                              \
  PTHREAD_CHECK (pthread_mutex_init (&(__LOCK__), NULL))
/* We recycle OpenCL objects by not actually freeing them until the
   very end. Thus, the lock should not be destroyed at the refcount 0. */
#define POCL_DESTROY_LOCK(__LOCK__)                                           \
  PTHREAD_CHECK (pthread_mutex_destroy (&(__LOCK__)))

/* If available, use an Adaptive mutex for locking in the pthread driver,
   otherwise fallback to simple mutexes */
#define POCL_FAST_LOCK_T pthread_mutex_t
#define POCL_FAST_LOCK(l) POCL_LOCK (l)
#define POCL_FAST_UNLOCK(l) POCL_UNLOCK (l)

#ifdef PTHREAD_ADAPTIVE_MUTEX_INITIALIZER_NP
#define POCL_FAST_INIT(l)                                                     \
  do                                                                          \
    {                                                                         \
      pthread_mutexattr_t attrs;                                              \
      pthread_mutexattr_init (&attrs);                                        \
      PTHREAD_CHECK (                                                         \
          pthread_mutexattr_settype (&attrs, PTHREAD_MUTEX_ADAPTIVE_NP));     \
      PTHREAD_CHECK (pthread_mutex_init (&l, &attrs));                        \
      PTHREAD_CHECK (pthread_mutexattr_destroy (&attrs));                     \
    }                                                                         \
  while (0)
#else
#define POCL_FAST_INIT(l) POCL_INIT_LOCK (l)
#endif

#define POCL_FAST_DESTROY(l) POCL_DESTROY_LOCK (l)

#define POCL_INIT_COND(c) PTHREAD_CHECK (pthread_cond_init (&c, NULL))
#define POCL_DESTROY_COND(c) PTHREAD_CHECK (pthread_cond_destroy (&c))
#define POCL_SIGNAL_COND(c) PTHREAD_CHECK (pthread_cond_signal (&c))
#define POCL_BROADCAST_COND(c) PTHREAD_CHECK (pthread_cond_broadcast (&c))
#define POCL_WAIT_COND(c, m) PTHREAD_CHECK (pthread_cond_wait (&c, &m))
#define POCL_TIMEDWAIT_COND(c, m, t)                                          \
  PTHREAD_CHECK2 (ETIMEDOUT, pthread_cond_timedwait (&c, &m, &t))

#define POCL_IGNORE_SIGNAL_IN_THREAD(s)                                       \
  do                                                                          \
    {                                                                         \
      sigset_t signal_mask;                                                   \
      sigemptyset (&signal_mask);                                             \
      sigaddset (&signal_mask, SIGPIPE);                                      \
      PTHREAD_CHECK (pthread_sigmask (SIG_BLOCK, &signal_mask, NULL));        \
    }                                                                         \
  while (0)

#define POCL_CREATE_THREAD(thr, func, arg)                                    \
  PTHREAD_CHECK (pthread_create (&thr, NULL, func, arg))
#define POCL_JOIN_THREAD(thr) PTHREAD_CHECK (pthread_join (thr, NULL))
#define POCL_JOIN_THREAD2(thr, res_ptr)                                       \
  PTHREAD_CHECK (pthread_join (thr, res_ptr))
#define POCL_EXIT_THREAD(res) pthread_exit (res)

#endif
