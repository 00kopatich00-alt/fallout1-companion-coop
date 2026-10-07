#include "plib/gnw/crash_handler.h"

#include <cstdarg>
#include <cstdio>
#include <ctime>

#if _WIN32

#include <windows.h>

#include <dbghelp.h>

#include <cstdio>
#include <cstring>
#include <ctime>

#pragma comment(lib, "dbghelp.lib")

namespace fallout {

void startup_log(const char* format, ...)
{
    static bool first = true;
    FILE* file = fopen("startup.log", first ? "wt" : "at");
    first = false;
    if (file == NULL) {
        return;
    }
    time_t now = time(NULL);
    struct tm* local = localtime(&now);
    char stamp[32] = "";
    if (local != NULL) {
        strftime(stamp, sizeof(stamp), "%H:%M:%S", local);
    }
    fprintf(file, "[%s] ", stamp);
    va_list args;
    va_start(args, format);
    vfprintf(file, format, args);
    va_end(args);
    fputc('\n', file);
    fclose(file);
}

// A failure the player must be told about. Used where the game used to just quit.
void startup_fail(const char* reason)
{
    startup_log("FAILED: %s", reason);
    char message[1024];
    snprintf(message, sizeof(message),
        "%s\n\nIf this keeps happening, please send the file startup.log (in the game's folder) to the mod author.",
        reason);
    MessageBoxA(NULL, message, "Fallout Coop", MB_OK | MB_ICONERROR);
}

static LONG WINAPI crash_handler_unhandled_exception_filter(EXCEPTION_POINTERS* exceptionPointers)
{
    time_t now = time(NULL);
    struct tm localTime;
    localtime_s(&localTime, &now);

    char timeBuf[32];
    strftime(timeBuf, sizeof(timeBuf), "%Y%m%d_%H%M%S", &localTime);

    char dumpPath[MAX_PATH];
    _snprintf_s(dumpPath, sizeof(dumpPath), _TRUNCATE, "crash_%s.dmp", timeBuf);

    HANDLE file = CreateFileA(dumpPath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION mdei;
        mdei.ThreadId = GetCurrentThreadId();
        mdei.ExceptionPointers = exceptionPointers;
        mdei.ClientPointers = FALSE;

        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file,
            (MINIDUMP_TYPE)(MiniDumpWithIndirectlyReferencedMemory | MiniDumpScanMemory),
            exceptionPointers != NULL ? &mdei : NULL, NULL, NULL);

        CloseHandle(file);
    }

    DWORD code = 0;
    void* addr = NULL;
    if (exceptionPointers != NULL && exceptionPointers->ExceptionRecord != NULL) {
        code = exceptionPointers->ExceptionRecord->ExceptionCode;
        addr = exceptionPointers->ExceptionRecord->ExceptionAddress;
    }

    char moduleName[MAX_PATH] = "unknown";
    DWORD64 offset = 0;
    if (addr != NULL) {
        HMODULE module = NULL;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                (LPCSTR)addr, &module)) {
            GetModuleFileNameA(module, moduleName, sizeof(moduleName));
            offset = (DWORD64)addr - (DWORD64)module;
        }
    }

    FILE* log = fopen("coopnet_debug.log", "a");
    if (log != NULL) {
        fprintf(log, "\nCRASH: unhandled exception code=0x%08lX at address=%p (module=%s+0x%llX) -- minidump written to %s\n",
            code, addr, moduleName, offset, dumpPath);

        if (exceptionPointers != NULL && exceptionPointers->ExceptionRecord != NULL
            && exceptionPointers->ExceptionRecord->NumberParameters >= 2) {
            ULONG_PTR kind = exceptionPointers->ExceptionRecord->ExceptionInformation[0];
            fprintf(log, "  access: %s address 0x%llX\n",
                kind == 0 ? "read" : (kind == 1 ? "write" : "execute"),
                (unsigned long long)exceptionPointers->ExceptionRecord->ExceptionInformation[1]);
        }

        // Where the crashing thread was (module+offset per frame, resolved with
        // the matching PDB), so a crash can be traced without the dump.
        if (exceptionPointers != NULL && exceptionPointers->ContextRecord != NULL) {
            CONTEXT context = *exceptionPointers->ContextRecord;
            HANDLE process = GetCurrentProcess();
            HANDLE thread = GetCurrentThread();
            SymInitialize(process, NULL, TRUE);

            STACKFRAME64 frame;
            memset(&frame, 0, sizeof(frame));
            frame.AddrPC.Offset = context.Rip;
            frame.AddrPC.Mode = AddrModeFlat;
            frame.AddrFrame.Offset = context.Rbp;
            frame.AddrFrame.Mode = AddrModeFlat;
            frame.AddrStack.Offset = context.Rsp;
            frame.AddrStack.Mode = AddrModeFlat;

            fprintf(log, "  stack (module+offset):\n");
            for (int depth = 0; depth < 40 && frame.AddrPC.Offset != 0; depth++) {
                char frameModule[MAX_PATH] = "unknown";
                DWORD64 frameOffset = frame.AddrPC.Offset;
                HMODULE module = NULL;
                if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                        (LPCSTR)frame.AddrPC.Offset, &module)) {
                    GetModuleFileNameA(module, frameModule, sizeof(frameModule));
                    frameOffset = frame.AddrPC.Offset - (DWORD64)module;
                }
                fprintf(log, "    %s+0x%llX\n", frameModule, frameOffset);

                if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame, &context, NULL,
                        SymFunctionTableAccess64, SymGetModuleBase64, NULL)) {
                    break;
                }
            }

            SymCleanup(process);
        }
        fclose(log);
    }

    // Coop: the whole point of this handler is getting a report back from
    // testers, who otherwise have no idea a silent close was even a crash
    // worth mentioning, let alone that it left files worth sending. A plain
    // MessageBox works from inside an exception filter (the crashing thread
    // is technically still alive, its state frozen, until this filter
    // returns) and needs nothing from the rest of the engine, which may
    // itself be in a broken state by now.
    char message[512];
    _snprintf_s(message, sizeof(message), _TRUNCATE,
        "Your game just crashed!\n\n"
        "Please send these two files (in this game's folder) to the mod author "
        "so this can get fixed:\n\n"
        "  coopnet_debug.log\n"
        "  %s\n",
        dumpPath);
    MessageBoxA(NULL, message, "Fallout Coop - Crash", MB_OK | MB_ICONERROR);

    return EXCEPTION_EXECUTE_HANDLER;
}

// Coop: a frozen game ("Not responding") leaves just as little behind as a
// crash does -- the log simply stops. A watchdog thread notices when the window
// has not handled messages for a while and writes where the main thread is
// stuck (module+offset per stack frame, to be looked up with the matching PDB)
// plus a minidump, then lets the game carry on.
static DWORD g_mainThreadId = 0;

static BOOL CALLBACK crash_handler_find_window(HWND hwnd, LPARAM param)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == GetCurrentProcessId() && IsWindowVisible(hwnd) && GetWindow(hwnd, GW_OWNER) == NULL) {
        *reinterpret_cast<HWND*>(param) = hwnd;
        return FALSE;
    }
    return TRUE;
}

static void crash_handler_report_hang()
{
    HANDLE thread = OpenThread(THREAD_ALL_ACCESS, FALSE, g_mainThreadId);
    if (thread == NULL) {
        return;
    }

    FILE* log = fopen("coopnet_debug.log", "a");
    if (log != NULL) {
        fprintf(log, "\nHANG: the game window has stopped responding. Main thread stack (module+offset):\n");
    }

    if (SuspendThread(thread) != (DWORD)-1) {
        CONTEXT context;
        memset(&context, 0, sizeof(context));
        context.ContextFlags = CONTEXT_FULL;
        if (GetThreadContext(thread, &context)) {
            HANDLE process = GetCurrentProcess();
            SymInitialize(process, NULL, TRUE);

            STACKFRAME64 frame;
            memset(&frame, 0, sizeof(frame));
            frame.AddrPC.Offset = context.Rip;
            frame.AddrPC.Mode = AddrModeFlat;
            frame.AddrFrame.Offset = context.Rbp;
            frame.AddrFrame.Mode = AddrModeFlat;
            frame.AddrStack.Offset = context.Rsp;
            frame.AddrStack.Mode = AddrModeFlat;

            for (int depth = 0; depth < 40; depth++) {
                if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame, &context, NULL,
                        SymFunctionTableAccess64, SymGetModuleBase64, NULL)
                    || frame.AddrPC.Offset == 0) {
                    break;
                }

                char moduleName[MAX_PATH] = "unknown";
                DWORD64 offset = frame.AddrPC.Offset;
                HMODULE module = NULL;
                if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                        (LPCSTR)frame.AddrPC.Offset, &module)) {
                    GetModuleFileNameA(module, moduleName, sizeof(moduleName));
                    offset = frame.AddrPC.Offset - (DWORD64)module;
                }
                if (log != NULL) {
                    fprintf(log, "  %s+0x%llX\n", moduleName, offset);
                }
            }

            SymCleanup(process);
        }
        ResumeThread(thread);
    }

    time_t now = time(NULL);
    struct tm localTime;
    localtime_s(&localTime, &now);
    char timeBuf[32];
    strftime(timeBuf, sizeof(timeBuf), "%Y%m%d_%H%M%S", &localTime);
    char dumpPath[MAX_PATH];
    _snprintf_s(dumpPath, sizeof(dumpPath), _TRUNCATE, "hang_%s.dmp", timeBuf);
    HANDLE file = CreateFileA(dumpPath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file != INVALID_HANDLE_VALUE) {
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file, MiniDumpNormal, NULL, NULL, NULL);
        CloseHandle(file);
        if (log != NULL) {
            fprintf(log, "HANG: minidump written to %s\n", dumpPath);
        }
    }

    if (log != NULL) {
        fclose(log);
    }
    CloseHandle(thread);
}

static DWORD WINAPI crash_handler_watchdog(LPVOID)
{
    int hungChecks = 0;
    bool reported = false;
    for (;;) {
        Sleep(2000);

        HWND window = NULL;
        EnumWindows(crash_handler_find_window, reinterpret_cast<LPARAM>(&window));
        if (window == NULL) {
            continue;
        }

        if (IsHungAppWindow(window)) {
            hungChecks++;
            // ~10 seconds of not answering, once per freeze.
            if (hungChecks >= 5 && !reported) {
                reported = true;
                crash_handler_report_hang();
            }
        } else {
            hungChecks = 0;
            reported = false;
        }
    }
    return 0;
}

static bool startup_file_exists(const char* path)
{
    DWORD attributes = GetFileAttributesA(path);
    return attributes != INVALID_FILE_ATTRIBUTES;
}

typedef LONG(WINAPI* RtlGetVersionFn)(OSVERSIONINFOW*);

static void startup_log_environment()
{
    char exePath[MAX_PATH] = "?";
    GetModuleFileNameA(NULL, exePath, sizeof(exePath));
    char cwd[MAX_PATH] = "?";
    GetCurrentDirectoryA(sizeof(cwd), cwd);
    startup_log("build %s %s", __DATE__, __TIME__);
    startup_log("exe: %s", exePath);
    startup_log("folder: %s", cwd);
    startup_log("command line: %s", GetCommandLineA());

    OSVERSIONINFOW version;
    memset(&version, 0, sizeof(version));
    version.dwOSVersionInfoSize = sizeof(version);
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    RtlGetVersionFn getVersion = ntdll != NULL ? reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion")) : NULL;
    if (getVersion != NULL && getVersion(&version) == 0) {
        startup_log("windows %lu.%lu build %lu", version.dwMajorVersion, version.dwMinorVersion, version.dwBuildNumber);
    }

    MEMORYSTATUSEX memory;
    memset(&memory, 0, sizeof(memory));
    memory.dwLength = sizeof(memory);
    if (GlobalMemoryStatusEx(&memory)) {
        startup_log("memory: %llu MB total, %llu MB free", memory.ullTotalPhys / (1024 * 1024), memory.ullAvailPhys / (1024 * 1024));
    }
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    startup_log("processors: %lu", info.dwNumberOfProcessors);

    // Which game files are here? A missing MASTER.DAT / CRITTER.DAT is the most
    // common reason a copy of the mod "does nothing".
    const char* files[] = { "MASTER.DAT", "CRITTER.DAT", "fallout.cfg", "f1_res.ini", "smooth_scaling.txt", "DATA", "SAVEGAME" };
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        startup_log("file %-18s %s", files[i], startup_file_exists(files[i]) ? "present" : "MISSING");
    }

    // Another copy of the game running makes this one quit at once (see winmain.cc).
    HANDLE probe = OpenMutexA(SYNCHRONIZE, FALSE, "GNW95MUTEX");
    if (probe != NULL) {
        startup_log("WARNING: another game window already holds GNW95MUTEX");
        CloseHandle(probe);
    }
}

void crash_handler_install()
{
    startup_log("---- starting ----");
    startup_log_environment();
    SetUnhandledExceptionFilter(crash_handler_unhandled_exception_filter);

    g_mainThreadId = GetCurrentThreadId();
    HANDLE watchdog = CreateThread(NULL, 0, crash_handler_watchdog, NULL, 0, NULL);
    if (watchdog != NULL) {
        CloseHandle(watchdog);
    }
}

} // namespace fallout

#else

namespace fallout {

void crash_handler_install()
{
}

void startup_log(const char* format, ...)
{
    (void)format;
}

void startup_fail(const char* reason)
{
    fprintf(stderr, "%s\n", reason);
}

} // namespace fallout

#endif
