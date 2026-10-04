#include "plib/gnw/crash_handler.h"

#if _WIN32

#include <windows.h>

#include <dbghelp.h>

#include <cstdio>
#include <cstring>
#include <ctime>

#pragma comment(lib, "dbghelp.lib")

namespace fallout {

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

void crash_handler_install()
{
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

} // namespace fallout

#endif
