#include "logging.h"

#include <windows.h>

#include <cstdio>
#include <iterator>

namespace crg {
	namespace {

		std::wstring g_logPath;

	} // namespace

	void InitializeLogging(const std::wstring& logPath) {
		g_logPath = logPath;
	}

	void Log(const wchar_t* message) {
		if (g_logPath.empty()) {
			return;
		}

		FILE* file = nullptr;
		if (_wfopen_s(&file, g_logPath.c_str(), L"a+, ccs=UTF-8") != 0 || file == nullptr) {
			return;
		}

		SYSTEMTIME now{};
		GetLocalTime(&now);
		std::fwprintf(file,
			L"%04u-%02u-%02u %02u:%02u:%02u.%03u  %ls\n",
			now.wYear, now.wMonth, now.wDay,
			now.wHour, now.wMinute, now.wSecond, now.wMilliseconds,
			message);
		std::fclose(file);
	}

	void LogFormat(const wchar_t* format, const wchar_t* value) {
		wchar_t buffer[1024]{};
		_snwprintf_s(buffer, std::size(buffer), _TRUNCATE, format, value);
		Log(buffer);
	}

} // namespace crg
