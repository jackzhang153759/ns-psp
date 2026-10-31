// Switch-only entry points that other translation units call into SDLMain.

#pragma once

#if PPSSPP_PLATFORM(SWITCH)

// Applies the authoritative LogFile setting once g_Config has been loaded.
//
// The early ini peek at the very start of main() is best-effort only: it depends on guessing where
// ppsspp.ini lives, which has broken twice already when the memstick location changed. Once the
// config is actually loaded this decides for real:
//  - enabled and not open yet: opens the timestamped log stream (append mode);
//  - disabled but open (the early guess was wrong): silences stdout/stderr without closing them,
//    so later printf()s can never crash.
// Either way the resulting state is shown on screen through the OSD, so "no log file" can never be
// a mystery again.
void SwitchApplyLogFileSetting(bool enabled);

#endif  // PPSSPP_PLATFORM(SWITCH)
