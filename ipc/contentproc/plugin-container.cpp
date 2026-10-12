/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "nsXPCOM.h"
#include "nsXULAppAPI.h"
#include "nsAutoPtr.h"

// JihadPmLogSem.c — C, and deliberately carries no header, so it drops unchanged into both
// this build and the daemon's. Declared here rather than in one because that is the entire
// interface: one call whose only job is to keep the archive member in the link.
extern "C" int jihad_pmlog_sem_installed(void);

#ifdef XP_WIN
#include <windows.h>
// we want a wmain entry point
// but we don't want its DLL load protection, because we'll handle it here
#define XRE_DONT_PROTECT_DLL_LOAD
#include "nsWindowsWMain.cpp"
#include "nsSetDllDirectory.h"
#else
// FIXME/cjones testing
#include <unistd.h>
#endif

// JIHAD DIAGNOSTIC (R7): the child process dies with SIGSEGV between a successful
// InitForChrome and handling its first IPC message, and the device's own minicore reporter
// is out of report slots ("no room for new reports"), so nothing records where. This prints
// the faulting address, a backtrace, and /proc/self/maps to stderr — which the child
// inherits from the daemon, so it lands in the variant's daemon.log. The maps dump is the
// part that matters on ARM: even a single frame is enough to name the faulting library.
#if defined(OS_POSIX) || defined(XP_UNIX)
#include <execinfo.h>
#include <signal.h>
#include <ucontext.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void
jihad_child_fault_handler(int sig, siginfo_t* info, void* ctx)
{
  void* frames[32];
  int n = backtrace(frames, 32);
  fprintf(stderr, "[jihad-npapi-child] FAULT sig=%d addr=%p pid=%d frames=%d\n",
          sig, info ? info->si_addr : nullptr, (int)getpid(), n);

  // backtrace() returns nothing useful from a signal frame on ARM (no unwind tables
  // through the trampoline), so take the registers straight out of the signal context.
  // pc and lr against the maps below give the faulting library and offset, and
  // addr2line on the unstripped build turns that into file:line.
#if defined(__arm__)
  if (ctx) {
    ucontext_t* uc = (ucontext_t*)ctx;
    fprintf(stderr, "[jihad-npapi-child] FAULT pc=%08lx lr=%08lx sp=%08lx r0=%08lx\n",
            (unsigned long)uc->uc_mcontext.arm_pc,
            (unsigned long)uc->uc_mcontext.arm_lr,
            (unsigned long)uc->uc_mcontext.arm_sp,
            (unsigned long)uc->uc_mcontext.arm_r0);
  }
#else
  (void)ctx;
#endif
  fflush(stderr);
  backtrace_symbols_fd(frames, n, STDERR_FILENO);

  FILE* maps = fopen("/proc/self/maps", "r");
  if (maps) {
    char line[512];
    fputs("[jihad-npapi-child] --- maps ---\n", stderr);
    while (fgets(line, sizeof(line), maps)) {
      // Only the executable mappings; the full map is hundreds of lines of noise.
      if (strstr(line, "r-xp")) {
        fputs("[jihad-npapi-child] ", stderr);
        fputs(line, stderr);
      }
    }
    fclose(maps);
  }
  fflush(stderr);

  // Restore the default disposition and re-raise so the exit status still says "crashed".
  signal(sig, SIG_DFL);
  raise(sig);
}

static void
jihad_install_child_fault_handler()
{
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_sigaction = jihad_child_fault_handler;
  sa.sa_flags = SA_SIGINFO;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGSEGV, &sa, nullptr);
  sigaction(SIGBUS, &sa, nullptr);
  sigaction(SIGILL, &sa, nullptr);
  sigaction(SIGFPE, &sa, nullptr);
}
#endif

#ifdef MOZ_GMP
#include "GMPLoader.h"

mozilla::gmp::SandboxStarter*
MakeSandboxStarter()
{
    return nullptr;
}
#endif

int
content_process_main(int argc, char* argv[])
{
    // Check for the absolute minimum number of args we need to move
    // forward here. We expect the last arg to be the child process type.
    if (argc < 1) {
      return 3;
    }

#if defined(OS_POSIX) || defined(XP_UNIX)
    jihad_install_child_fault_handler();
#endif

    // JIHAD (R7): force JihadPmLogSem.o into the link and say so. It interposes sem_open for
    // the single name "PmLogLib", without which dlopening the device's Flash deadlocks this
    // process forever — libPmLogLib's constructor waits on a named semaphore created by
    // webOS's glibc 2.8, whose sem_t value word our glibc 2.23 reads as "no tokens, waiters
    // pending". The call is also the only cheap proof the interposer survived the link: it
    // lives in a static archive, and an archive member with no undefined symbol to satisfy
    // is silently dropped. See render/goanna/JihadPmLogSem.c.
    fprintf(stderr, "[jihad-npapi-child] pmlog sem interposition=%d\n",
            jihad_pmlog_sem_installed());

    // JIHAD DIAGNOSTIC (R7): the child's own view of its argv. Doubles as the control that
    // child stderr reaches the parent's log at all — every other child probe is worthless
    // until this line is seen.
    fprintf(stderr, "[jihad-npapi-child] main argc=%d\n", argc);
    for (int i = 0; i < argc; i++) {
      fprintf(stderr, "[jihad-npapi-child]   argv[%d]=%s\n", i, argv[i]);
    }
    fflush(stderr);

#ifdef MOZ_GMP
    XREChildData childData;
#endif

    XRE_SetProcessType(argv[--argc]);

#ifdef XP_WIN
    // For plugins, this is done in PluginProcessChild::Init, as we need to
    // avoid it for unsupported plugins.  See PluginProcessChild::Init for
    // the details.
    if (XRE_GetProcessType() != GeckoProcessType_Plugin) {
        mozilla::SanitizeEnvironmentVariables();
        SetDllDirectoryW(L"");
    }
#endif
#ifdef MOZ_PLUGIN_CONTAINER
#ifdef MOZ_GMP
    // On desktop, the GMPLoader lives in plugin-container, so that its
    // code can be covered by an EME/GMP vendor's voucher.
    nsAutoPtr<mozilla::gmp::SandboxStarter> starter(MakeSandboxStarter());
    if (XRE_GetProcessType() == GeckoProcessType_GMPlugin) {
        childData.gmpLoader = mozilla::gmp::CreateGMPLoader(starter);
    }
    nsresult rv = XRE_InitChildProcess(argc, argv, &childData);
#else
    nsresult rv = XRE_InitChildProcess(argc, argv);
#endif
    NS_ENSURE_SUCCESS(rv, 1);
#endif

    return 0;
}
