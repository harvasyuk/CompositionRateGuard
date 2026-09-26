#pragma once

#include <sal.h>

#include <string>

namespace crg {

	// Starts a new log when the existing one has grown too large; the previous
	// log is kept alongside it with an .old suffix.
	void InitializeLogging(const std::wstring& logPath);
	void Log(const wchar_t* message);
	void LogFormat(_Printf_format_string_ const wchar_t* format, ...);

} // namespace crg
