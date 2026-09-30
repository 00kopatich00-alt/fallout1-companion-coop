#include "plib/gnw/crash_handler.h"

#if _WIN32

#include <windows.h>

#include <dbghelp.h>

#include <cstdio>
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

void crash_handler_install()
{
    SetUnhandledExceptionFilter(crash_handler_unhandled_exception_filter);
}

} // namespace fallout

#else

namespace fallout {

void crash_handler_install()
{
}

} // namespace fallout

#endif
