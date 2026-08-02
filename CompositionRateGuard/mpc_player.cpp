#include "mpc_player.h"

#include "app_constants.h"
#include "logging.h"
#include "settings.h"

#include <commdlg.h>
#include <dwmapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cwctype>
#include <iterator>
#include <string>
#include <vector>

namespace crg {
	namespace {

		constexpr size_t kLongPathCapacity = 32768;

		std::wstring ToLower(std::wstring value) {
			std::transform(value.begin(), value.end(), value.begin(),
				[](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
			return value;
		}

		std::wstring GetBaseName(const std::wstring& path) {
			const size_t slash = path.find_last_of(L"\\/");
			return slash == std::wstring::npos ? path : path.substr(slash + 1);
		}

		bool IsMpcHcExecutableName(const std::wstring& executableName) {
			const std::wstring name = ToLower(executableName);
			// Support standard names plus renamed variants such as mpc-hc64_nvo.exe.
			return name.size() >= 10 && name.rfind(L"mpc-hc", 0) == 0 &&
				name.compare(name.size() - 4, 4, L".exe") == 0;
		}

		bool IsExistingFile(const std::wstring& path) {
			const DWORD attributes = GetFileAttributesW(path.c_str());
			return attributes != INVALID_FILE_ATTRIBUTES &&
				(attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
		}

		std::wstring ReadAppPath(const wchar_t* executableName) {
			const std::wstring subkey =
				L"Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\\" +
				std::wstring(executableName);

			for (HKEY root : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE}) {
				std::vector<wchar_t> path(kLongPathCapacity);
				DWORD size = static_cast<DWORD>(path.size() * sizeof(wchar_t));
				if (RegGetValueW(root, subkey.c_str(), nullptr, RRF_RT_REG_SZ,
					nullptr, path.data(), &size) == ERROR_SUCCESS &&
					IsExistingFile(path.data())) {
					return path.data();
				}
			}
			return {};
		}

		std::wstring FindMpcExecutable(const std::wstring& configuredPath) {
			if (IsExistingFile(configuredPath)) {
				return configuredPath;
			}

			for (const wchar_t* name : { L"mpc-hc64.exe", L"mpc-hc.exe" }) {
				const std::wstring appPath = ReadAppPath(name);
				if (!appPath.empty()) {
					return appPath;
				}

				std::vector<wchar_t> found(kLongPathCapacity);
				const DWORD foundLength = SearchPathW(
					nullptr, name, nullptr, static_cast<DWORD>(found.size()),
					found.data(), nullptr);
				if (foundLength > 0 && foundLength < found.size() &&
					IsExistingFile(found.data())) {
					return found.data();
				}
			}

			struct CandidateRoot {
				const wchar_t* environmentVariable;
				const wchar_t* relativePath;
			};
			const CandidateRoot candidates[] = {
				{L"ProgramFiles", L"MPC-HC\\mpc-hc64.exe"},
				{L"ProgramFiles", L"MPC-HC\\mpc-hc.exe"},
				{L"ProgramFiles(x86)", L"MPC-HC\\mpc-hc.exe"},
				{L"LOCALAPPDATA", L"MPC-HC\\mpc-hc64.exe"},
			};
			for (const CandidateRoot& candidate : candidates) {
				std::vector<wchar_t> root(kLongPathCapacity);
				const DWORD length = GetEnvironmentVariableW(
					candidate.environmentVariable, root.data(), static_cast<DWORD>(root.size()));
				if (length == 0 || length >= root.size()) {
					continue;
				}

				std::wstring path(root.data(), length);
				if (!path.empty() && path.back() != L'\\') {
					path.push_back(L'\\');
				}
				path += candidate.relativePath;
				if (IsExistingFile(path)) {
					return path;
				}
			}
			return {};
		}

		std::wstring ChooseMpcExecutable(HWND owner) {
			std::vector<wchar_t> path(kLongPathCapacity);
			OPENFILENAMEW dialog{};
			dialog.lStructSize = sizeof(dialog);
			dialog.hwndOwner = owner;
			dialog.lpstrFilter =
				L"MPC-HC executable (mpc-hc*.exe)\0mpc-hc*.exe\0"
				L"Executables (*.exe)\0*.exe\0\0";
			dialog.lpstrFile = path.data();
			dialog.nMaxFile = static_cast<DWORD>(path.size());
			dialog.lpstrTitle = L"Locate MPC-HC";
			dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
			return GetOpenFileNameW(&dialog) ? std::wstring(path.data()) : std::wstring();
		}

		std::wstring QuoteCommandLineArgument(const std::wstring& argument) {
			std::wstring quoted = L"\"";
			size_t backslashes = 0;
			for (const wchar_t ch : argument) {
				if (ch == L'\\') {
					++backslashes;
				}
				else if (ch == L'\"') {
					quoted.append(backslashes * 2 + 1, L'\\');
					quoted.push_back(ch);
					backslashes = 0;
				}
				else {
					quoted.append(backslashes, L'\\');
					backslashes = 0;
					quoted.push_back(ch);
				}
			}
			quoted.append(backslashes * 2, L'\\');
			quoted.push_back(L'\"');
			return quoted;
		}

		bool IsMpcHcProcess(DWORD pid) {
			HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
			if (process == nullptr) {
				return false;
			}

			std::vector<wchar_t> path(kLongPathCapacity);
			DWORD size = static_cast<DWORD>(path.size());
			const BOOL success = QueryFullProcessImageNameW(process, 0, path.data(), &size);
			CloseHandle(process);
			return success && IsMpcHcExecutableName(GetBaseName(std::wstring(path.data(), size)));
		}

		RECT GetWindowBounds(HWND hwnd) {
			RECT bounds{};
			if (FAILED(DwmGetWindowAttribute(
				hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &bounds, sizeof(bounds)))) {
				GetWindowRect(hwnd, &bounds);
			}
			return bounds;
		}

		long long RectArea(const RECT& rect) {
			const long long width = std::max<LONG>(0, rect.right - rect.left);
			const long long height = std::max<LONG>(0, rect.bottom - rect.top);
			return width * height;
		}

		struct FindPlayerContext {
			HWND overlayWindow = nullptr;
			PlayerWindow best;
		};

		BOOL CALLBACK FindPlayerWindowCallback(HWND hwnd, LPARAM parameter) {
			auto* context = reinterpret_cast<FindPlayerContext*>(parameter);
			PlayerWindow& best = context->best;

			if (!IsWindowVisible(hwnd) || IsIconic(hwnd) || hwnd == context->overlayWindow) {
				return TRUE;
			}

			const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
			if ((style & WS_CHILD) != 0) {
				return TRUE;
			}

			DWORD pid = 0;
			GetWindowThreadProcessId(hwnd, &pid);
			if (pid == 0 || !IsMpcHcProcess(pid)) {
				return TRUE;
			}

			const RECT bounds = GetWindowBounds(hwnd);
			const long long area = RectArea(bounds);
			if (area <= best.area) {
				return TRUE;
			}

			const HMONITOR monitorHandle = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
			MONITORINFO monitorInfo{};
			monitorInfo.cbSize = sizeof(monitorInfo);
			if (!GetMonitorInfoW(monitorHandle, &monitorInfo)) {
				return TRUE;
			}

			best.hwnd = hwnd;
			best.pid = pid;
			best.bounds = bounds;
			best.monitor = monitorInfo.rcMonitor;
			best.area = area;
			best.style = style;
			best.exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
			return TRUE;
		}

	} // namespace

	bool LaunchMediaFile(
		HWND owner,
		const std::wstring& mediaPath,
		Settings& settings,
		const std::wstring& iniPath) {
		std::wstring executable = FindMpcExecutable(settings.playerPath);
		if (executable.empty()) {
			executable = ChooseMpcExecutable(owner);
			if (executable.empty()) {
				Log(L"MPC-HC selection was cancelled; media was not opened.");
				return false;
			}
			settings.playerPath = executable;
			SaveSettings(iniPath, settings);
		}
		else if (settings.playerPath != executable) {
			settings.playerPath = executable;
			SaveSettings(iniPath, settings);
		}

		std::wstring commandLine =
			QuoteCommandLineArgument(executable) + L" " + QuoteCommandLineArgument(mediaPath);
		std::vector<wchar_t> mutableCommandLine(commandLine.begin(), commandLine.end());
		mutableCommandLine.push_back(L'\0');

		STARTUPINFOW startup{};
		startup.cb = sizeof(startup);
		PROCESS_INFORMATION process{};
		if (!CreateProcessW(executable.c_str(), mutableCommandLine.data(), nullptr, nullptr,
			FALSE, 0, nullptr, nullptr, &startup, &process)) {
			const DWORD error = GetLastError();
			wchar_t message[512]{};
			_snwprintf_s(message, std::size(message), _TRUNCATE,
				L"MPC-HC could not be started (Win32 error %lu).", error);
			Log(message);
			MessageBoxW(owner, message, kAppName, MB_OK | MB_ICONERROR);
			return false;
		}

		CloseHandle(process.hThread);
		CloseHandle(process.hProcess);
		LogFormat(L"Asked MPC-HC to open: %ls", mediaPath.c_str());
		return true;
	}

	bool IsAnyMpcHcProcessRunning() {
		HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
		if (snapshot == INVALID_HANDLE_VALUE) {
			// Staying alive is safer than exiting while playback might be active.
			return true;
		}

		PROCESSENTRY32W entry{};
		entry.dwSize = sizeof(entry);
		bool found = false;
		if (Process32FirstW(snapshot, &entry)) {
			do {
				if (IsMpcHcExecutableName(entry.szExeFile)) {
					found = true;
					break;
				}
			} while (Process32NextW(snapshot, &entry));
		}
		CloseHandle(snapshot);
		return found;
	}

	PlayerWindow FindPlayerWindow(HWND overlayWindow) {
		FindPlayerContext context{};
		context.overlayWindow = overlayWindow;
		EnumWindows(FindPlayerWindowCallback, reinterpret_cast<LPARAM>(&context));
		return context.best;
	}

	bool IsBorderlessFullscreenWindow(const PlayerWindow& player) {
		if (player.hwnd == nullptr) {
			return false;
		}

		constexpr LONG tolerance = 12;
		const bool edgesMatch =
			std::abs(player.bounds.left - player.monitor.left) <= tolerance &&
			std::abs(player.bounds.top - player.monitor.top) <= tolerance &&
			std::abs(player.bounds.right - player.monitor.right) <= tolerance &&
			std::abs(player.bounds.bottom - player.monitor.bottom) <= tolerance;

		const long long monitorArea = RectArea(player.monitor);
		const double coverage = monitorArea > 0
			? static_cast<double>(RectArea(player.bounds)) / static_cast<double>(monitorArea)
			: 0.0;

		const bool coversMonitor = edgesMatch || coverage >= 0.985;
		const bool popup = (player.style & WS_POPUP) != 0;
		const bool hasCaption = (player.style & WS_CAPTION) == WS_CAPTION;
		const bool hasThickFrame = (player.style & WS_THICKFRAME) != 0;
		const bool borderlessStyle = popup || (!hasCaption && !hasThickFrame);
		return coversMonitor && borderlessStyle;
	}

	void LogPlayerState(const PlayerWindow& player, const wchar_t* state) {
		if (player.hwnd == nullptr) {
			Log(state);
			return;
		}

		wchar_t className[256]{};
		GetClassNameW(player.hwnd, className, static_cast<int>(std::size(className)));

		wchar_t message[1024]{};
		_snwprintf_s(
			message, std::size(message), _TRUNCATE,
			L"%ls hwnd=0x%p pid=%lu class=%ls style=0x%llX exStyle=0x%llX "
			L"bounds=(%ld,%ld)-(%ld,%ld) monitor=(%ld,%ld)-(%ld,%ld).",
			state,
			player.hwnd,
			player.pid,
			className,
			static_cast<unsigned long long>(player.style),
			static_cast<unsigned long long>(player.exStyle),
			player.bounds.left, player.bounds.top,
			player.bounds.right, player.bounds.bottom,
			player.monitor.left, player.monitor.top,
			player.monitor.right, player.monitor.bottom);
		Log(message);
	}

} // namespace crg
