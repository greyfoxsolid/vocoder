// dv_mode.h - entry point of the helper's "dv" mode (D-Star / P25 codecs).
// See dv_mode.cpp. GPL v3 or later; see ../NOTICE.md.
#pragma once

// Runs the dv request loop on stdin/stdout until 'Q' or stdin closes. [step]
// is the helper's crash-line step variable, updated so a crash line names the
// dv operation it happened in. Returns the process exit code. The caller must
// already have put stdin/stdout into binary mode on Windows.
int qdv_dv_main(volatile const char** step);
