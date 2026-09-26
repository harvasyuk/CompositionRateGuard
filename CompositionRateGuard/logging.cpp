#include "logging.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <iterator>

namespace crg {
	namespace {

		constexpr ULONGLONG kMaxLogBytes = 1024 * 1024;

		std::wstring g_logPath;

		void RotateLogIfTooLarge(const std::wstring& logPath) {
			WIN32_FILE_ATTRIBUTE_DATA attributes{};
			if (!GetFileAttributesExW(logPath.c_str(), GetFileExInfoStandard, &attributes)) {
				return;
			}

			const ULONGLONG size =
				(static_cast<ULONGLONG>(attributes.nFileSizeHigh) << 32) | attributes.nFileSizeLow;
			if (size > kMaxLogBytes) {
				MoveFileExW(logPath.c_str(), (logPath + L".old").c_str(), MOVEFILE_REPLACE_EXISTING);
			}
		}

	} // namespace

	void InitializeLogging(const std::wstring& logPath) {
		RotateLogIfTooLarge(logPath);
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

	void LogFormat(const wchar_t* format, ...) {
		wchar_t buffer[1024]{};
		va_list arguments;
		va_start(arguments, format);
		_vsnwprintf_s(buffer, std::size(buffer), _TRUNCATE, format, arguments);
		va_end(arguments);
		Log(buffer);
	}

} // namespace crg
