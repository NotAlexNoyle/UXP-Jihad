/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Jihad Browser — the /PmLogLib named-semaphore ABI trap.
 *
 * Compiled into every one of our binaries that can dlopen a device NPAPI plugin: the
 * daemon (its plugin scan) and plugin-container (the plugin itself). Plain C, no headers
 * from UXP or from the daemon, so the same file drops into both build systems unchanged.
 */

// ── WHAT THIS IS FOR ────────────────────────────────────────────────────────────────────
//
// The device's libflashplayer.so has libWebKitLuna.so as a DT_NEEDED, which drags in
// libPmLogLib.so. PmLogLib's ELF constructor opens the POSIX named semaphore "PmLogLib"
// and sem_wait()s it to guard a SysV shared-memory table of log contexts that every
// process on the device shares.
//
// glibc changed sem_t's layout in 2.21. The value word went from a raw token count to
// (tokens << 1) | has-waiters. webOS 3's userland is glibc 2.8 — the OLD layout — and
// everything we cross-build runs on the bundled glibc 2.23, the NEW one. /dev/shm/
// sem.PmLogLib is created once per boot by whichever process opens it first, in that
// process's layout, and then outlives every user of it.
//
// When a system process wins that race the word holds a raw 1, meaning "one token". Our
// 2.23 sem_wait reads the same word as ZERO tokens with the waiters bit set, so it goes
// straight to futex_wait and never returns. dlopen never completes and whoever called it
// wedges: measured on device 2026-08-10 as
//     futex(0x2dd98000, FUTEX_WAIT_BITSET_PRIVATE|FUTEX_CLOCK_REALTIME, 1, NULL, ffffffff
// immediately after the mmap of /dev/shm/sem.PmLogLib, with every library already loaded.
//
// That is why this reads as an intermittent, reboot-triggered failure on binaries nobody
// touched: it is decided by a boot race, not by our code. The same dlopen run under the
// device's own /lib/ld-linux.so.3 completes and returns both Flash MIME types.
//
// ── WHY INTERPOSE RATHER THAN REPAIR THE SEMAPHORE ──────────────────────────────────────
//
// Rewriting /dev/shm/sem.PmLogLib into our layout, or posting it once to make it readable
// to us, both work — and both hand every OTHER process on the device an extra token on a
// lock guarding a shared table. That silently weakens PmLogLib's mutual exclusion
// system-wide to fix a browser. Interposing ONE name does not: PmLogLib gets a semaphore
// that never leaves this process, so no other glibc ever parses its layout, and the file
// in /dev/shm is not touched at all.
//
// Everything else is forwarded to the real implementation untouched, which MATTERS: our
// own shared frame buffers are named semaphores too (/dev/shm/sem.browserserver.<pid>-<n>,
// created here and opened by the adapter inside LunaSysMgr), and those genuinely must stay
// shared across the process boundary.

#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdarg.h>
#include <stddef.h>
#include <string.h>

// PmLogLib asks for the name WITHOUT a leading slash (verified on device: the interposer
// logged sem_open("PmLogLib")). POSIX says a portable name starts with '/', and glibc
// accepts both, so match both rather than trusting either spelling.
// Link anchor. plugin-container builds this TU into a static archive, and an archive member
// is only pulled in to satisfy an UNDEFINED symbol — sem_open is already defined by
// libpthread, so nothing was undefined and the whole file was silently dropped from the
// link. Calling this from the program's entry point forces the member in. The daemon links
// the object directly and does not need it; it costs one call there.
__attribute__((visibility("default")))
int jihad_pmlog_sem_installed(void) { return 1; }

static int jihad_is_pmlog_name(const char* name)
{
  if (!name) return 0;
  if (name[0] == '/') ++name;
  return strcmp(name, "PmLogLib") == 0;
}

static sem_t g_pmlogSem;
static pthread_once_t g_pmlogSemOnce = PTHREAD_ONCE_INIT;
static unsigned int g_pmlogSemInitial = 1;

static void jihad_pmlog_sem_init(void)
{
  // pshared = 0. Process-private is the whole point: it can never be observed, and so can
  // never be misread, by a glibc with a different sem_t layout.
  sem_init(&g_pmlogSem, 0, g_pmlogSemInitial);
}

__attribute__((visibility("default")))
sem_t* sem_open(const char* name, int oflag, ...)
{
  static sem_t* (*real_sem_open)(const char*, int, ...);
  if (!real_sem_open)
    real_sem_open = (sem_t* (*)(const char*, int, ...))dlsym(RTLD_NEXT, "sem_open");

  // The mode/value pair is only present when O_CREAT is set; reading it otherwise walks off
  // the end of the argument list.
  mode_t mode = 0;
  unsigned int value = 0;
  const int creating = (oflag & O_CREAT) != 0;
  if (creating) {
    va_list ap;
    va_start(ap, oflag);
    mode = (mode_t)va_arg(ap, unsigned int);
    value = va_arg(ap, unsigned int);
    va_end(ap);
  }

  if (jihad_is_pmlog_name(name)) {
    if (creating) g_pmlogSemInitial = value;
    pthread_once(&g_pmlogSemOnce, jihad_pmlog_sem_init);
    return &g_pmlogSem;
  }

  if (!real_sem_open) return SEM_FAILED;
  if (creating) return real_sem_open(name, oflag, mode, value);
  return real_sem_open(name, oflag);
}

// PmLogLib closes the semaphore on unload. glibc's sem_close looks the pointer up in its
// table of NAMED semaphores, would not find ours, and would return EINVAL — harmless today,
// but only by luck. Answer for our own pointer and forward everything else.
__attribute__((visibility("default")))
int sem_close(sem_t* sem)
{
  static int (*real_sem_close)(sem_t*);
  if (!real_sem_close)
    real_sem_close = (int (*)(sem_t*))dlsym(RTLD_NEXT, "sem_close");

  // Deliberately NOT sem_destroy: the plugin can be unloaded and reloaded within one
  // process (the scan does exactly that), and a destroyed static would be reused.
  if (sem == &g_pmlogSem) return 0;

  if (!real_sem_close) return 0;
  return real_sem_close(sem);
}
