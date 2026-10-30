#pragma once
void LogOpen(const char* path);
void Log(const char* fmt, ...);
const char* LogPath();   // full path of the log file in use
