#include "mpc_player.h"

#include "app_constants.h"
#include "logging.h"

#include <commdlg.h>
#include <dwmapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cwctype>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace crg {
	namespace {

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

		// Also accept the executable the user configured, which may have any name.
		bool IsPlayerExecutableName(const std::wstring& executableName, const std::wstring& playerPath) {
			return IsMpcHcExecutableName(executableName) ||
				(!playerPath.empty() && ToLower(executableName) == ToLower(GetBaseName(playerPath)));
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

		bool IsPlayerProcess(DWORD pid, const std::wstring& playerPath) {
			HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
			if (process == nullptr) {
				return false;
			}

			// Nearly all image paths fit in MAX_PATH; only allocate for long ones.
			std::wstring imagePath;
			wchar_t shortPath[MAX_PATH];
			DWORD size = static_cast<DWORD>(std::size(shortPath));
			if (QueryFullProcessImageNameW(process, 0, shortPath, &size)) {
				imagePath.assign(shortPath, size);
			}
			else if (GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
				std::vector<wchar_t> longPath(kLongPathCapacity);
				size = static_cast<DWORD>(longPath.size());
				if (QueryFullProcessImageNameW(process, 0, longPath.data(), &size)) {
					imagePath.assign(longPath.data(), size);
				}
			}
			CloseHandle(process);
			return !imagePath.empty() &&
				IsPlayerExecutableName(GetBaseName(imagePath), playerPath);
		}

		// Windows on another virtual desktop, or hidden by the shell, still report
		// as visible but are not on screen.
		bool IsCloaked(HWND hwnd) {
			DWORD cloaked = 0;
			return SUCCEEDED(DwmGetWindowAttribute(
				hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked != 0;
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
			const std::wstring* playerPath = nullptr;
			PlayerWindow best;
			// Processes already checked during this enumeration; many windows share
			// a process. PID reuse within one short pass is not a practical concern.
			std::vector<std::pair<DWORD, bool>> checkedProcesses;
		};

		bool IsPlayerProcessCached(FindPlayerContext& context, DWORD pid) {
			for (const auto& [checkedPid, isPlayer] : context.checkedProcesses) {
				if (checkedPid == pid) {
					return isPlayer;
				}
			}
			const bool isPlayer = IsPlayerProcess(pid, *context.playerPath);
			context.checkedProcesses.emplace_back(pid, isPlayer);
			return isPlayer;
		}

		BOOL CALLBACK FindPlayerWindowCallback(HWND hwnd, LPARAM parameter) {
			auto* context = reinterpret_cast<FindPlayerContext*>(parameter);
			PlayerWindow& best = context->best;

			if (!IsWindowVisible(hwnd) || IsIconic(hwnd) || hwnd == context->overlayWindow) {
				return TRUE;
			}

			const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
			if ((style & WS_CHILD) != 0 || IsCloaked(hwnd)) {
				return TRUE;
			}

			// Cheap size check first: opening the process is the expensive part.
			const RECT bounds = GetWindowBounds(hwnd);
			const long long area = RectArea(bounds);
			if (area <= best.area) {
				return TRUE;
			}

			DWORD pid = 0;
			GetWindowThreadProcessId(hwnd, &pid);
			if (pid == 0 || !IsPlayerProcessCached(*context, pid)) {
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

	bool LaunchMediaFile(HWND owner, const std::wstring& mediaPath, std::wstring& playerPath) {
		std::wstring executable = FindMpcExecutable(playerPath);
		if (executable.empty()) {
			executable = ChooseMpcExecutable(owner);
			if (executable.empty()) {
				Log(L"MPC-HC selection was cancelled; media was not opened.");
				return false;
			}
		}
		playerPath = executable;

		const std::wstring commandLine =
			QuoteCommandLineArgument(executable) + L" " + QuoteCommandLineArgument(mediaPath);
		std::vector<wchar_t> mutableCommandLine(commandLine.begin(), commandLine.end());
		mutableCommandLine.push_back(L'\0');

		STARTUPINFOW startup{};
		startup.cb = sizeof(startup);
		PROCESS_INFORMATION process{};
		// Start suspended so the foreground right is granted before MPC-HC can
		// create its window.
		if (!CreateProcessW(executable.c_str(), mutableCommandLine.data(), nullptr, nullptr,
			FALSE, CREATE_SUSPENDED, nullptr, nullptr, &startup, &process)) {
			const DWORD error = GetLastError();
			wchar_t message[128]{};
			_snwprintf_s(message, std::size(message), _TRUNCATE,
				L"MPC-HC could not be started (Win32 error %lu).", error);
			Log(message);
			MessageBoxW(owner, message, kAppName, MB_OK | MB_ICONERROR);
			return false;
		}

		// Fails harmlessly when the guard itself has no right to pass on.
		AllowSetForegroundWindow(process.dwProcessId);
		if (ResumeThread(process.hThread) == static_cast<DWORD>(-1)) {
			// Never leave a suspended player behind.
			TerminateProcess(process.hProcess, 1);
			CloseHandle(process.hThread);
			CloseHandle(process.hProcess);
			Log(L"MPC-HC could not be resumed after starting.");
			MessageBoxW(owner, L"MPC-HC could not be started.", kAppName, MB_OK | MB_ICONERROR);
			return false;
		}
		CloseHandle(process.hThread);
		CloseHandle(process.hProcess);
		LogFormat(L"Asked MPC-HC to open: %ls", mediaPath.c_str());
		return true;
	}

	bool IsAnyPlayerProcessRunning(const std::wstring& playerPath) {
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
				if (IsPlayerExecutableName(entry.szExeFile, playerPath)) {
					found = true;
					break;
				}
			} while (Process32NextW(snapshot, &entry));
		}
		CloseHandle(snapshot);
		return found;
	}

	PlayerWindow FindPlayerWindow(HWND overlayWindow, const std::wstring& playerPath) {
		FindPlayerContext context{};
		context.overlayWindow = overlayWindow;
		context.playerPath = &playerPath;
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

		LogFormat(
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
	}

} // namespace crg
