#pragma once

void InitLarecompLogging();
void ShutdownLarecompLogging();

// The file the log sink appends to, relative to the working folder as InitLarecompLogging chose it
// (LARECOMP_LOG_FILE or logs/larecomp_NNN.log). Empty before InitLarecompLogging has run.
const char* LarecompLogFilePath();
