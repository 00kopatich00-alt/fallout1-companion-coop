#ifndef FALLOUT_PLIB_GNW_CRASH_HANDLER_H_
#define FALLOUT_PLIB_GNW_CRASH_HANDLER_H_

namespace fallout {

// Coop: installs a Windows crash handler that writes a minidump (crash_*.dmp,
// next to the exe) and a one-line summary (faulting module+offset) to
// coopnet_debug.log right before the process dies. Without this, a native
// crash (access violation, etc.) leaves no trace at all -- the debug log
// just silently stops, which is exactly what made a coop tester's crash
// report undiagnosable. No-op on non-Windows platforms.
void crash_handler_install();

} // namespace fallout

#endif /* FALLOUT_PLIB_GNW_CRASH_HANDLER_H_ */
