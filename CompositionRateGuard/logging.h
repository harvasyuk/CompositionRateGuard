#pragma once

#include <string>

namespace crg {

	void InitializeLogging(const std::wstring& logPath);
	void Log(const wchar_t* message);
	void LogFormat(const wchar_t* format, const wchar_t* value);

} // namespace crg
