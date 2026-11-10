// Crash reports: when the program fails, save a small dump next to the log and
// tell the user where it is, instead of the window just disappearing.
#pragma once

namespace crash {

// Installs the handlers. Reports go to %LOCALAPPDATA%\AirWindows\crashes; the
// five newest are kept. `interactive` is false for unattended runs
// (--exit-after), which save the report and end without a dialog.
void Install(bool interactive);

// Deliberate failures for --selftest-crash.
[[noreturn]] void TestAccessViolation();
[[noreturn]] void TestTerminate();

}  // namespace crash
