/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "nsNPAPIPlugin.h"
#include "nsNPAPIPluginInstance.h"
#include "nsIMemory.h"
#include "nsPluginsDir.h"
#include "nsPluginsDirUtils.h"
#include "prmem.h"
#include "prenv.h"
#include "prerror.h"
#include "prio.h"
#include "prthread.h"
#include "nsTArray.h"
#include <sys/stat.h>
#include "nsString.h"
#include "nsIFile.h"
#include "nsIPrefBranch.h"
#include "nsIPrefService.h"
#include "nsReadableUtils.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;

#define LOCAL_PLUGIN_DLL_SUFFIX ".so"
#ifdef XP_SOLARIS
#define DEFAULT_X11_PATH "/usr/openwin/lib"
#elif defined(LINUX)
#define DEFAULT_X11_PATH "/usr/X11R6/lib/"
#elif defined(__APPLE__)
#define DEFAULT_X11_PATH "/usr/X11R6/lib"
#undef LOCAL_PLUGIN_DLL_SUFFIX
#define LOCAL_PLUGIN_DLL_SUFFIX ".dylib"
#define LOCAL_PLUGIN_DLL_ALT_SUFFIX ".so"
#else
#define DEFAULT_X11_PATH ""
#endif

#if (MOZ_WIDGET_GTK == 2)

#define PLUGIN_MAX_LEN_OF_TMP_ARR 512

static void DisplayPR_LoadLibraryErrorMessage(const char *libName)
{
    char errorMsg[PLUGIN_MAX_LEN_OF_TMP_ARR] = "Cannot get error from NSPR.";
    if (PR_GetErrorTextLength() < (int) sizeof(errorMsg))
        PR_GetErrorText(errorMsg);

    fprintf(stderr, "LoadPlugin: failed to initialize shared library %s [%s]\n",
        libName, errorMsg);
}

static void SearchForSoname(const char* name, char** soname)
{
    if (!(name && soname))
        return;
    PRDir *fdDir = PR_OpenDir(DEFAULT_X11_PATH);
    if (!fdDir)
        return;       

    int n = strlen(name);
    PRDirEntry *dirEntry;
    while ((dirEntry = PR_ReadDir(fdDir, PR_SKIP_BOTH))) {
        if (!PL_strncmp(dirEntry->name, name, n)) {
            if (dirEntry->name[n] == '.' && dirEntry->name[n+1] && !dirEntry->name[n+2]) {
                // name.N, wild guess this is what we need
                char out[PLUGIN_MAX_LEN_OF_TMP_ARR] = DEFAULT_X11_PATH;
                PL_strcat(out, dirEntry->name);
                *soname = PL_strdup(out);
               break;
            }
        }
    }

    PR_CloseDir(fdDir);
}

static bool LoadExtraSharedLib(const char *name, char **soname, bool tryToGetSoname)
{
    bool ret = true;
    PRLibSpec tempSpec;
    PRLibrary *handle;
    tempSpec.type = PR_LibSpec_Pathname;
    tempSpec.value.pathname = name;
    handle = PR_LoadLibraryWithFlags(tempSpec, PR_LD_NOW|PR_LD_GLOBAL);
    if (!handle) {
        ret = false;
        DisplayPR_LoadLibraryErrorMessage(name);
        if (tryToGetSoname) {
            SearchForSoname(name, soname);
            if (*soname) {
                ret = LoadExtraSharedLib((const char *) *soname, nullptr, false);
            }
        }
    }
    return ret;
}

#define PLUGIN_MAX_NUMBER_OF_EXTRA_LIBS 32
#define PREF_PLUGINS_SONAME "plugin.soname.list"
#ifdef XP_SOLARIS
#define DEFAULT_EXTRA_LIBS_LIST "libXt" LOCAL_PLUGIN_DLL_SUFFIX ":libXext" LOCAL_PLUGIN_DLL_SUFFIX ":libXm" LOCAL_PLUGIN_DLL_SUFFIX
#else
#define DEFAULT_EXTRA_LIBS_LIST "libXt" LOCAL_PLUGIN_DLL_SUFFIX ":libXext" LOCAL_PLUGIN_DLL_SUFFIX
#endif
/*
 this function looks for
 user_pref("plugin.soname.list", "/usr/X11R6/lib/libXt.so.6:libXext.so");
 in user's pref.js
 and loads all libs in specified order
*/

static void LoadExtraSharedLibs()
{
    // check out if user's prefs.js has libs name
    nsresult res;
    nsCOMPtr<nsIPrefBranch> prefs(do_GetService(NS_PREFSERVICE_CONTRACTID, &res));
    if (NS_SUCCEEDED(res) && (prefs != nullptr)) {
        char *sonameList = nullptr;
        bool prefSonameListIsSet = true;
        res = prefs->GetCharPref(PREF_PLUGINS_SONAME, &sonameList);
        if (!sonameList) {
            // pref is not set, lets use hardcoded list
            prefSonameListIsSet = false;
            sonameList = PL_strdup(DEFAULT_EXTRA_LIBS_LIST);
        }
        if (sonameList) {
            char *arrayOfLibs[PLUGIN_MAX_NUMBER_OF_EXTRA_LIBS] = {0};
            int numOfLibs = 0;
            char *nextToken;
            char *p = nsCRT::strtok(sonameList,":",&nextToken);
            if (p) {
                while (p && numOfLibs < PLUGIN_MAX_NUMBER_OF_EXTRA_LIBS) {
                    arrayOfLibs[numOfLibs++] = p;
                    p = nsCRT::strtok(nextToken,":",&nextToken);
                }
            } else // there is just one lib
                arrayOfLibs[numOfLibs++] = sonameList;

            char sonameListToSave[PLUGIN_MAX_LEN_OF_TMP_ARR] = "";
            for (int i=0; i<numOfLibs; i++) {
                // trim out head/tail white spaces (just in case)
                bool head = true;
                p = arrayOfLibs[i];
                while (*p) {
                    if (*p == ' ' || *p == '\t') {
                        if (head) {
                            arrayOfLibs[i] = ++p;
                        } else {
                            *p = 0;
                        }
                    } else {
                        head = false;
                        p++;
                    }
                }
                if (!arrayOfLibs[i][0]) {
                    continue; // null string
                }
                bool tryToGetSoname = true;
                if (PL_strchr(arrayOfLibs[i], '/')) {
                    //assuming it's real name, try to stat it
                    struct stat st;
                    if (stat((const char*) arrayOfLibs[i], &st)) {
                        //get just a file name
                        arrayOfLibs[i] = PL_strrchr(arrayOfLibs[i], '/') + 1;
                    } else
                        tryToGetSoname = false;
                }
                char *soname = nullptr;
                if (LoadExtraSharedLib(arrayOfLibs[i], &soname, tryToGetSoname)) {
                    //construct soname's list to save in prefs
                    p = soname ? soname : arrayOfLibs[i];
                    int n = PLUGIN_MAX_LEN_OF_TMP_ARR -
                        (strlen(sonameListToSave) + strlen(p));
                    if (n > 0) {
                        PL_strcat(sonameListToSave, p);
                        PL_strcat(sonameListToSave,":");
                    }
                    if (soname) {
                        PL_strfree(soname); // it's from strdup
                    }
                    if (numOfLibs > 1)
                        arrayOfLibs[i][strlen(arrayOfLibs[i])] = ':'; //restore ":" in sonameList
                }
            }

            // Check whether sonameListToSave is a empty String, Bug: 329205
            if (sonameListToSave[0]) 
                for (p = &sonameListToSave[strlen(sonameListToSave) - 1]; *p == ':'; p--)
                    *p = 0; //delete tail ":" delimiters

            if (!prefSonameListIsSet || PL_strcmp(sonameList, sonameListToSave)) {
                // if user specified some bogus soname I overwrite it here,
                // otherwise it'll decrease performance by calling popen() in SearchForSoname
                // every time for each bogus name
                prefs->SetCharPref(PREF_PLUGINS_SONAME, (const char *)sonameListToSave);
            }
            PL_strfree(sonameList);
        }
    }
}
#endif //MOZ_WIDGET_GTK == 2

/* nsPluginsDir implementation */

bool nsPluginsDir::IsPluginFile(nsIFile* file)
{
    nsAutoCString filename;
    if (NS_FAILED(file->GetNativeLeafName(filename)))
        return false;

    NS_NAMED_LITERAL_CSTRING(dllSuffix, LOCAL_PLUGIN_DLL_SUFFIX);
    if (filename.Length() > dllSuffix.Length() &&
        StringEndsWith(filename, dllSuffix))
        return true;
    
#ifdef LOCAL_PLUGIN_DLL_ALT_SUFFIX
    NS_NAMED_LITERAL_CSTRING(dllAltSuffix, LOCAL_PLUGIN_DLL_ALT_SUFFIX);
    if (filename.Length() > dllAltSuffix.Length() &&
        StringEndsWith(filename, dllAltSuffix))
        return true;
#endif
    return false;
}

/* nsPluginFile implementation */

nsPluginFile::nsPluginFile(nsIFile* file)
: mPlugin(file)
{
}

nsPluginFile::~nsPluginFile()
{
}

nsresult nsPluginFile::LoadPlugin(PRLibrary **outLibrary)
{
    PRLibSpec libSpec;
    libSpec.type = PR_LibSpec_Pathname;
    bool exists = false;
    mPlugin->Exists(&exists);
    if (!exists)
        return NS_ERROR_FILE_NOT_FOUND;

    nsresult rv;
    nsAutoCString path;
    rv = mPlugin->GetNativePath(path);
    if (NS_FAILED(rv))
        return rv;

    libSpec.value.pathname = path.get();

#if (MOZ_WIDGET_GTK == 2)

    // Normally, Mozilla isn't linked against libXt and libXext
    // since it's a Gtk/Gdk application.  On the other hand,
    // legacy plug-ins expect the libXt and libXext symbols
    // to already exist in the global name space.  This plug-in
    // wrapper is linked against libXt and libXext, but since
    // we never call on any of these libraries, plug-ins still
    // fail to resolve Xt symbols when trying to do a dlopen
    // at runtime.  Explicitly opening Xt/Xext into the global
    // namespace before attempting to load the plug-in seems to
    // work fine.
#if defined(XP_SOLARIS)
    // Acrobat/libXm: Lazy resolving might cause crash later (bug 211587)
    *outLibrary = PR_LoadLibraryWithFlags(libSpec, PR_LD_NOW);
    pLibrary = *outLibrary;
#else
    // Some dlopen() doesn't recover from a failed PR_LD_NOW (bug 223744)
    *outLibrary = PR_LoadLibraryWithFlags(libSpec, 0);
    pLibrary = *outLibrary;
#endif    
    if (!pLibrary) {
        LoadExtraSharedLibs();
        // try reload plugin once more
        *outLibrary = PR_LoadLibraryWithFlags(libSpec, 0);
        pLibrary = *outLibrary;
        if (!pLibrary) {
            DisplayPR_LoadLibraryErrorMessage(libSpec.value.pathname);
            return NS_ERROR_FAILURE;
        }
    }
#else
    *outLibrary = PR_LoadLibraryWithFlags(libSpec, 0);
    pLibrary = *outLibrary;
#endif  // MOZ_WIDGET_GTK == 2

#ifdef DEBUG
    printf("LoadPlugin() %s returned %lx\n", 
           libSpec.value.pathname, (unsigned long)pLibrary);
#endif

    if (!pLibrary) {
        return NS_ERROR_FAILURE;
    }
    
    return NS_OK;
}

// JIHAD (cavekit-flash.md R10, T-580): the library read by plugin-container -jihad-plugin-scan, not loaded here.
// Where the bundled loader launches the children (the device), every plugin folder library is read in a process
// of its own under a time limit (JIHAD_PLUGIN_SCAN_MS, 10 s): one that hangs is killed, one that crashes or is not
// a library is simply not a plugin, and none of them is ever mapped into the daemon. JIHAD_PLUGIN_SCAN_OOP=0 is the
// old way. Returns NS_ERROR_NOT_AVAILABLE when the out-of-process scan is not in use, and NS_ERROR_ABORT when the scan
// could not judge the library (the scanner did not start, did not finish in time, or was killed from outside): the
// caller must not record such a library as invalid, or a scan made while the device swapped would leave Flash out
// until pluginreg.dat was deleted (the inspector's review, 2026-11-21, F-001). A library that timed out is not scanned
// again in this process (each try would cost the whole limit); the next start tries it afresh.
static nsTArray<nsCString>* sJihadScanTimedOut;

static nsresult
JihadScanPluginOutOfProcess(const nsACString& aPath, nsPluginInfo& info)
{
    const char* loader = PR_GetEnv("MOZ_CHILD_PROCESS_LOADER");
    const char* greDir = PR_GetEnv("MOZ_CHILD_PROCESS_LOADER_PATH");
    const char* off = PR_GetEnv("JIHAD_PLUGIN_SCAN_OOP");
    if (!loader || !*loader || !greDir || !*greDir || (off && !strcmp(off, "0"))) {
        return NS_ERROR_NOT_AVAILABLE;
    }
    const char* msEnv = PR_GetEnv("JIHAD_PLUGIN_SCAN_MS");
    const int limitMs = (msEnv && atoi(msEnv) > 0) ? atoi(msEnv) : 10000;
    nsAutoCString child(greDir);
    child.AppendLiteral("/plugin-container");
    nsAutoCString path(aPath);
    if (sJihadScanTimedOut && sJihadScanTimedOut->Contains(path)) {
        fprintf(stderr, "[jihad-npapi] plugin scan: %s: timed out earlier in this process: not judged\n", path.get());
        return NS_ERROR_ABORT;
    }
    char* argv[] = { const_cast<char*>(loader), const_cast<char*>("--library-path"), const_cast<char*>(greDir),
                     const_cast<char*>(child.get()), const_cast<char*>("-jihad-plugin-scan"),
                     const_cast<char*>(path.get()), nullptr };
    int fds[2];
    if (pipe(fds) != 0) {
        fprintf(stderr, "[jihad-npapi] plugin scan: %s: no pipe (%d): not judged\n", path.get(), errno);
        return NS_ERROR_ABORT;
    }
    fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, fds[1], 1);
    posix_spawn_file_actions_addclose(&fa, fds[1]);
    pid_t pid = -1;
    const PRIntervalTime t0 = PR_IntervalNow();
    int err = posix_spawn(&pid, loader, &fa, nullptr, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    close(fds[1]);
    if (err != 0) {
        close(fds[0]);
        fprintf(stderr, "[jihad-npapi] plugin scan: %s: the scanner did not start (%d): not judged\n", path.get(), err);
        return NS_ERROR_ABORT;
    }
    // read until the scanner's last line ("end"), its end of output, or the limit; a scanner that closes its output
    // and keeps running, or leaves a child holding it, is reaped below under the same limit (F-002)
    nsAutoCString out;
    bool timedOut = false;
    for (;;) {
        const int left = limitMs - (int)PR_IntervalToMilliseconds(PR_IntervalNow() - t0);
        if (left <= 0) {
            timedOut = true;
            break;
        }
        struct pollfd pfd = { fds[0], POLLIN, 0 };
        const int r = poll(&pfd, 1, left);
        if (r < 0 && errno == EINTR) {
            continue;
        }
        if (r <= 0) {
            timedOut = (r == 0);
            break;
        }
        char buf[1024];
        const ssize_t n = read(fds[0], buf, sizeof buf);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            break;
        }
        out.Append(buf, n);
        if (StringEndsWith(out, NS_LITERAL_CSTRING("end\n")) || out.Length() > 65536) {
            break;
        }
    }
    close(fds[0]);
    // the scanner reaped within what is left of the limit, killed if it is still there at its end
    int status = 0;
    bool reaped = false, lost = false;
    while (!timedOut) {
        const pid_t w = waitpid(pid, &status, WNOHANG);
        if (w == pid) {
            reaped = true;
            break;
        }
        if (w < 0 && errno != EINTR) {
            // reaped by someone else (ECHILD): its number may be another process's by now, so no signal goes to it,
            // and the output alone decides
            lost = true;
            status = 0;
            break;
        }
        const int left = limitMs - (int)PR_IntervalToMilliseconds(PR_IntervalNow() - t0);
        if (left <= 0) {
            timedOut = true;
            break;
        }
        PR_Sleep(PR_MillisecondsToInterval(left < 20 ? left : 20));
    }
    if (!reaped && !lost) {
        kill(pid, SIGKILL);
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    }
    const uint32_t ms = PR_IntervalToMilliseconds(PR_IntervalNow() - t0);
    // not judged: the limit, or a SIGKILL from outside (the kernel's OOM killer under swap): tried again next start
    if (timedOut || (WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL)) {
        if (timedOut) {
            if (!sJihadScanTimedOut) {
                sJihadScanTimedOut = new nsTArray<nsCString>();
            }
            sJihadScanTimedOut->AppendElement(path);
        }
        fprintf(stderr, "[jihad-npapi] plugin scan: %s: not judged (%s, %u ms): left out of this start only\n", path.get(),
                timedOut ? "the scanner did not finish in time, killed" : "the scanner was killed", ms);
        return NS_ERROR_ABORT;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0 || !StringEndsWith(out, NS_LITERAL_CSTRING("end\n"))) {
        fprintf(stderr, "[jihad-npapi] plugin scan: %s: not a plugin (%s, %u ms): left out\n", path.get(),
                WIFSIGNALED(status) ? "the scanner died by a signal" : "the scanner refused it", ms);
        return NS_ERROR_FAILURE;
    }
    nsAutoCString mime, name, description, version;
    int32_t start = 0;
    while (start < int32_t(out.Length())) {
        int32_t nl = out.FindChar('\n', start);
        if (nl < 0) {
            break;
        }
        const nsDependentCSubstring line(out, start, nl - start);
        start = nl + 1;
        int32_t tab = line.FindChar('\t');
        if (tab < 0) {
            continue;
        }
        const nsDependentCSubstring key(line, 0, tab);
        const nsDependentCSubstring value(line, tab + 1);
        if (key.EqualsLiteral("mime")) mime = value;
        else if (key.EqualsLiteral("name")) name = value;
        else if (key.EqualsLiteral("description")) description = value;
        else if (key.EqualsLiteral("version")) version = value;
    }
    if (mime.IsEmpty()) {
        fprintf(stderr, "[jihad-npapi] plugin scan: %s: no MIME types: left out\n", path.get());
        return NS_ERROR_FAILURE;
    }
    nsresult rv = ParsePluginMimeDescription(mime.get(), info);
    if (NS_FAILED(rv)) {
        return rv;
    }
    info.fFullPath = PL_strdup(path.get());
    int32_t slash = path.RFindChar('/');
    info.fFileName = PL_strdup(slash >= 0 ? path.get() + slash + 1 : path.get());
    info.fName = PL_strdup(name.IsEmpty() ? info.fFileName : name.get());
    info.fDescription = PL_strdup(description.get());
    info.fVersion = version.IsEmpty() ? nullptr : PL_strdup(version.get());
    fprintf(stderr, "[jihad-npapi] plugin scan: %s: \"%s\" %s, read out of process in %u ms\n", path.get(),
            info.fName, version.get(), ms);
    return NS_OK;
}

nsresult nsPluginFile::GetPluginInfo(nsPluginInfo& info, PRLibrary **outLibrary)
{
    *outLibrary = nullptr;

    info.fVersion = nullptr;

    {
        nsAutoCString scanPath;
        if (NS_SUCCEEDED(mPlugin->GetNativePath(scanPath))) {
            nsresult srv = JihadScanPluginOutOfProcess(scanPath, info);
            if (srv != NS_ERROR_NOT_AVAILABLE) {
                return srv;   // read, or left out; *outLibrary stays null: nothing of it is in this process
            }
        }
    }

    // Sadly we have to load the library for this to work.
    nsresult rv = LoadPlugin(outLibrary);
    if (NS_FAILED(rv))
        return rv;
  
    const char* (*npGetPluginVersion)() =
        (const char* (*)()) PR_FindFunctionSymbol(pLibrary, "NP_GetPluginVersion");
    if (npGetPluginVersion) {
        info.fVersion = PL_strdup(npGetPluginVersion());
    }

    const char* (*npGetMIMEDescription)() =
        (const char* (*)()) PR_FindFunctionSymbol(pLibrary, "NP_GetMIMEDescription");
    if (!npGetMIMEDescription) {
        return NS_ERROR_FAILURE;
    }

    const char* mimedescr = npGetMIMEDescription();
    if (!mimedescr) {
        return NS_ERROR_FAILURE;
    }

    rv = ParsePluginMimeDescription(mimedescr, info);
    if (NS_FAILED(rv)) {
        return rv;
    }

    nsAutoCString path;
    if (NS_FAILED(rv = mPlugin->GetNativePath(path)))
        return rv;
    info.fFullPath = PL_strdup(path.get());

    nsAutoCString fileName;
    if (NS_FAILED(rv = mPlugin->GetNativeLeafName(fileName)))
        return rv;
    info.fFileName = PL_strdup(fileName.get());

    NP_GetValueFunc npGetValue = (NP_GetValueFunc)PR_FindFunctionSymbol(pLibrary, "NP_GetValue");
    if (!npGetValue) {
        return NS_ERROR_FAILURE;
    }

    const char *name = nullptr;
    npGetValue(nullptr, NPPVpluginNameString, &name);
    if (name) {
        info.fName = PL_strdup(name);
    }
    else {
        info.fName = PL_strdup(fileName.get());
    }

    const char *description = nullptr;
    npGetValue(nullptr, NPPVpluginDescriptionString, &description);
    if (description) {
        info.fDescription = PL_strdup(description);
    }
    else {
        info.fDescription = PL_strdup("");
    }

    return NS_OK;
}

nsresult nsPluginFile::FreePluginInfo(nsPluginInfo& info)
{
    if (info.fName != nullptr)
        PL_strfree(info.fName);

    if (info.fDescription != nullptr)
        PL_strfree(info.fDescription);

    for (uint32_t i = 0; i < info.fVariantCount; i++) {
        if (info.fMimeTypeArray[i] != nullptr)
            PL_strfree(info.fMimeTypeArray[i]);

        if (info.fMimeDescriptionArray[i] != nullptr)
            PL_strfree(info.fMimeDescriptionArray[i]);

        if (info.fExtensionArray[i] != nullptr)
            PL_strfree(info.fExtensionArray[i]);
    }

    PR_FREEIF(info.fMimeTypeArray);
    PR_FREEIF(info.fMimeDescriptionArray);
    PR_FREEIF(info.fExtensionArray);

    if (info.fFullPath != nullptr)
        PL_strfree(info.fFullPath);

    if (info.fFileName != nullptr)
        PL_strfree(info.fFileName);

    if (info.fVersion != nullptr)
        PL_strfree(info.fVersion);

    return NS_OK;
}
