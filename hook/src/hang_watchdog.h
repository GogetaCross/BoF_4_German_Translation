// Hang watchdog — answers "where is the game actually stuck?" instead of us
// guessing from the tail of the log.
//
// The GOG-Galaxy-launch hang produces no error and no crash: the log simply
// stops mid-startup and the process idles. From outside there is no way to tell
// a blocked render thread from a game-logic wait, so this samples the process
// itself: if Present stops being called for long enough after rendering had
// already started, it walks EVERY thread in the process, records where each one
// is, and writes that to d3d9_hook.log.
//
// Safety rules this module keeps (a watchdog that deadlocks is worse than none):
//   - Never logs, allocates or resolves symbols while a thread is suspended.
//     Raw data is captured, the thread is resumed, and only then formatted.
//   - Reads foreign stacks through ReadProcessMemory so a bad ESP cannot fault.
//   - Reports at most a few times, then stays quiet.
#pragma once

// Start the watchdog thread. No-op unless the hang_watchdog config flag is on.
void hang_watchdog_install();
