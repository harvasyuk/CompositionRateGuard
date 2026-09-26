#include "application.h"

#include "app_constants.h"
#include "app_types.h"
#include "composition_rate.h"
#include "logging.h"
#include "mpc_player.h"
#include "overlay.h"
#include "resource.h"
#include "settings.h"

#include <shellapi.h>

#include <cstdio>
#include <deque>
#include <iterator>
#include <string>
#include <vector>

namespace crg {
	namespace {

		constexpr UINT kTrayCallback = WM_APP + 1;
		constexpr UINT kOpenPendingMedia = WM_APP + 2;
		constexpr UINT kForwardTimeoutMs = 5000;
		constexpr ULONG_PTR kOpenMediaCopyDataId = 0x4352474D; // "CRGM"
		constexpr UINT_PTR kPollTimerId = 1;
		constexpr UINT kPollIntervalMs = 1000;

		constexpr UINT kCmdEnabled = 1001;
		constexpr UINT kCmdStartWithWindows = 1002;
		constexpr UINT kCmdNearInvisible = 1010;
		constexpr UINT kCmdVisibleTest = 1011;
		constexpr UINT kCmdExit = 1099;

		enum class DetectionState : int {
			Disabled = 0,
			NoPlayerWindow = 1,
			PlayerWindowed = 2,
			BorderlessFullscreen = 3,
		};

		HINSTANCE g_instance = nullptr;
		HWND g_controlWindow = nullptr;
		HANDLE g_singleInstanceMutex = nullptr;
		UINT g_taskbarCreatedMessage = 0;
		HICON g_largeIcon = nullptr;
		HICON g_smallIcon = nullptr;

		Settings g_settings;
		Overlay g_overlay;
		DetectionState g_detectionState = DetectionState::NoPlayerWindow;
		std::wstring g_configDirectory;
		std::wstring g_iniPath;
		bool g_exitWhenPlayersClose = false;
		ULONGLONG g_lastPlayerLaunchTick = 0;
		// Media forwarded by other instances, launched outside WM_COPYDATA.
		std::deque<std::wstring> g_pendingMediaPaths;
		// Last rate logged while the guard is disabled; reset on enable so each
		// disabled period starts by logging its initial rate.
		CompositionRate g_lastLoggedRate;
		bool g_hasLoggedRate = false;

		void SetDetectionState(DetectionState state, const PlayerWindow& player) {
			if (g_detectionState == state) {
				return;
			}

			g_detectionState = state;
			switch (state) {
			case DetectionState::Disabled:
				Log(L"Detection state: disabled.");
				break;
			case DetectionState::NoPlayerWindow:
				Log(L"Detection state: no visible MPC-HC window.");
				break;
			case DetectionState::PlayerWindowed:
				LogPlayerState(player, L"Detection state: MPC-HC found, but not borderless fullscreen;");
				break;
			case DetectionState::BorderlessFullscreen:
				LogPlayerState(player, L"Detection state: borderless fullscreen MPC-HC detected;");
				break;
			}
		}

		bool LaunchRequestedMedia(const std::wstring& mediaPath) {
			const std::wstring previousPlayerPath = g_settings.playerPath;
			const bool launched = LaunchMediaFile(g_controlWindow, mediaPath, g_settings.playerPath);
			if (g_settings.playerPath != previousPlayerPath) {
				SaveSettings(g_iniPath, g_settings);
			}
			if (!launched) {
				return false;
			}

			// Refresh the startup grace period for association-scoped instances.
			// A persistent instance ignores this timestamp but can still launch media.
			g_lastPlayerLaunchTick = GetTickCount64();
			return true;
		}

		void LogCompositionRateChange() {
			const CompositionRate rate = QueryCompositionRate();
			if (g_hasLoggedRate && rate.SameAs(g_lastLoggedRate)) {
				return;
			}

			wchar_t text[128]{};
			FormatCompositionRate(rate, text, std::size(text));
			if (g_hasLoggedRate) {
				LogFormat(L"%ls (changed from %.3f Hz).", text, g_lastLoggedRate.ComposeHz());
			}
			else {
				LogFormat(L"%ls.", text);
			}

			g_lastLoggedRate = rate;
			g_hasLoggedRate = true;
		}

		void PollPlayer() {
			if (g_exitWhenPlayersClose &&
				GetTickCount64() - g_lastPlayerLaunchTick >= 2000 &&
				!IsAnyPlayerProcessRunning(g_settings.playerPath)) {
				Log(L"No MPC-HC process remains; exiting association-launched guard.");
				DestroyWindow(g_controlWindow);
				return;
			}

			if (!g_settings.enabled) {
				SetDetectionState(DetectionState::Disabled, PlayerWindow{});
				g_overlay.Hide();
				// Without the guard running, record when the unguarded fallback happens.
				LogCompositionRateChange();
				return;
			}
			g_hasLoggedRate = false;

			const PlayerWindow player = FindPlayerWindow(g_overlay.Window(), g_settings.playerPath);
			if (player.hwnd == nullptr) {
				SetDetectionState(DetectionState::NoPlayerWindow, player);
				g_overlay.Hide();
				return;
			}

			if (!IsBorderlessFullscreenWindow(player)) {
				SetDetectionState(DetectionState::PlayerWindowed, player);
				g_overlay.Hide();
				return;
			}

			// Tray interactions and utility windows must not momentarily tear down the
			// guard, so MPC-HC does not need to remain the foreground process.
			SetDetectionState(DetectionState::BorderlessFullscreen, player);
			g_overlay.ShowFor(player);
		}

		void AddTrayIcon() {
			NOTIFYICONDATAW data{};
			data.cbSize = sizeof(data);
			data.hWnd = g_controlWindow;
			data.uID = 1;
			data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
			data.uCallbackMessage = kTrayCallback;
			data.hIcon = g_smallIcon;
			wcscpy_s(data.szTip, kAppName);
			Shell_NotifyIconW(NIM_ADD, &data);
		}

		void RemoveTrayIcon() {
			NOTIFYICONDATAW data{};
			data.cbSize = sizeof(data);
			data.hWnd = g_controlWindow;
			data.uID = 1;
			Shell_NotifyIconW(NIM_DELETE, &data);
		}

		const wchar_t* CurrentStatusText() {
			switch (g_detectionState) {
			case DetectionState::Disabled:
				return L"Status: disabled";
			case DetectionState::NoPlayerWindow:
				return L"Status: no MPC-HC window";
			case DetectionState::PlayerWindowed:
				return L"Status: MPC-HC is not borderless fullscreen";
			case DetectionState::BorderlessFullscreen:
				return g_overlay.IsVisible()
					? L"Status: guarding borderless fullscreen MPC-HC"
					: L"Status: borderless fullscreen detected";
			default:
				return L"Status: waiting";
			}
		}

		void ShowTrayMenu() {
			POINT cursor{};
			GetCursorPos(&cursor);

			HMENU menu = CreatePopupMenu();
			if (menu == nullptr) {
				return;
			}

			// Opening any menu makes DWM restore its composition rate, so only the
			// display refresh rate is meaningful here.
			const CompositionRate rate = QueryCompositionRate();
			wchar_t rateText[128]{};
			if (rate.valid) {
				_snwprintf_s(rateText, std::size(rateText), _TRUNCATE,
					L"Display refresh: %.3f Hz", rate.RefreshHz());
			}
			else {
				wcscpy_s(rateText, L"Display refresh: unavailable");
			}

			AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, CurrentStatusText());
			AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, rateText);
			AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
			AppendMenuW(
				menu,
				MF_STRING | (g_settings.enabled ? MF_CHECKED : MF_UNCHECKED),
				kCmdEnabled,
				L"Enabled");
			AppendMenuW(
				menu,
				MF_STRING | (IsStartupEnabled() ? MF_CHECKED : MF_UNCHECKED),
				kCmdStartWithWindows,
				L"Start with Windows");

			HMENU modeMenu = CreatePopupMenu();
			AppendMenuW(
				modeMenu,
				MF_STRING | (g_settings.mode == OverlayMode::NearInvisible ? MF_CHECKED : MF_UNCHECKED),
				kCmdNearInvisible,
				L"Near-invisible 1 x 1 (default)");
			AppendMenuW(
				modeMenu,
				MF_STRING | (g_settings.mode == OverlayMode::VisibleTest ? MF_CHECKED : MF_UNCHECKED),
				kCmdVisibleTest,
				L"Visible test rectangle");
			AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(modeMenu), L"Guard mode");

			AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
			AppendMenuW(menu, MF_STRING, kCmdExit, L"Exit");

			SetForegroundWindow(g_controlWindow);
			TrackPopupMenu(
				menu,
				TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_LEFTALIGN,
				cursor.x,
				cursor.y,
				0,
				g_controlWindow,
				nullptr);
			PostMessageW(g_controlWindow, WM_NULL, 0, 0);
			DestroyMenu(menu);
		}

		void SetMode(OverlayMode mode) {
			if (g_settings.mode == mode) {
				return;
			}

			g_settings.mode = mode;
			SaveSettings(g_iniPath, g_settings);
			g_overlay.SetMode(mode);

			switch (mode) {
			case OverlayMode::NearInvisible:
				Log(L"Mode changed to near-invisible 1x1 surface (alpha 1/255).");
				break;
			case OverlayMode::VisibleTest:
				Log(L"Mode changed to visible test rectangle.");
				break;
			}
			PollPlayer();
		}

		LRESULT CALLBACK ControlWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
			if (message == g_taskbarCreatedMessage && g_taskbarCreatedMessage != 0) {
				AddTrayIcon();
				return 0;
			}

			switch (message) {
			case WM_COPYDATA: {
				const auto* data = reinterpret_cast<const COPYDATASTRUCT*>(lParam);
				if (data == nullptr || data->dwData != kOpenMediaCopyDataId ||
					data->lpData == nullptr || data->cbData < sizeof(wchar_t) ||
					data->cbData % sizeof(wchar_t) != 0) {
					return FALSE;
				}

				const size_t characterCount = data->cbData / sizeof(wchar_t);
				const auto* path = static_cast<const wchar_t*>(data->lpData);
				if (path[characterCount - 1] != L'\0') {
					return FALSE;
				}

				// Launching can show a file dialog or an error box. Do it after
				// returning, so the forwarding instance is not blocked meanwhile.
				g_pendingMediaPaths.emplace_back(path);
				if (g_pendingMediaPaths.size() == 1) {
					PostMessageW(hwnd, kOpenPendingMedia, 0, 0);
				}
				return TRUE;
			}
			case kOpenPendingMedia:
				while (!g_pendingMediaPaths.empty()) {
					const std::wstring mediaPath = std::move(g_pendingMediaPaths.front());
					g_pendingMediaPaths.pop_front();
					LaunchRequestedMedia(mediaPath);
				}
				return 0;
			case WM_CREATE:
				SetTimer(hwnd, kPollTimerId, kPollIntervalMs, nullptr);
				return 0;
			case WM_TIMER:
				if (wParam == kPollTimerId) {
					PollPlayer();
				}
				return 0;
			case kTrayCallback:
				if (lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU || lParam == WM_LBUTTONUP) {
					ShowTrayMenu();
				}
				return 0;
			case WM_COMMAND:
				switch (LOWORD(wParam)) {
				case kCmdEnabled:
					g_settings.enabled = !g_settings.enabled;
					SaveSettings(g_iniPath, g_settings);
					Log(g_settings.enabled ? L"Guard enabled." : L"Guard disabled.");
					PollPlayer();
					return 0;
				case kCmdStartWithWindows: {
					const bool enable = !IsStartupEnabled();
					if (SetStartupEnabled(enable)) {
						Log(enable ? L"Start with Windows enabled." : L"Start with Windows disabled.");
					}
					else {
						Log(L"Failed to change Start with Windows setting.");
						MessageBoxW(
							hwnd,
							L"Windows could not update the startup setting.",
							kAppName,
							MB_OK | MB_ICONERROR);
					}
					return 0;
				}
				case kCmdNearInvisible:
					SetMode(OverlayMode::NearInvisible);
					return 0;
				case kCmdVisibleTest:
					SetMode(OverlayMode::VisibleTest);
					return 0;
				case kCmdExit:
					DestroyWindow(hwnd);
					return 0;
				default:
					break;
				}
				break;
			case WM_DESTROY:
				KillTimer(hwnd, kPollTimerId);
				g_overlay.Hide();
				RemoveTrayIcon();
				PostQuitMessage(0);
				return 0;
			default:
				break;
			}
			return DefWindowProcW(hwnd, message, wParam, lParam);
		}

		bool RegisterWindowClasses() {
			WNDCLASSEXW controlClass{};
			controlClass.cbSize = sizeof(controlClass);
			controlClass.hInstance = g_instance;
			controlClass.lpfnWndProc = ControlWindowProc;
			controlClass.lpszClassName = kControlClass;
			controlClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
			controlClass.hIcon = g_largeIcon;
			controlClass.hIconSm = g_smallIcon;

			if (RegisterClassExW(&controlClass) == 0) {
				return false;
			}
			return g_overlay.RegisterWindowClass(g_instance);
		}

		bool CreateApplicationWindows() {
			g_controlWindow = CreateWindowExW(
				WS_EX_TOOLWINDOW,
				kControlClass,
				kAppName,
				WS_OVERLAPPED,
				0, 0, 0, 0,
				nullptr,
				nullptr,
				g_instance,
				nullptr);
			return g_controlWindow != nullptr && g_overlay.Create(g_instance, g_settings.mode);
		}

		std::vector<std::wstring> GetMediaArguments() {
			int argumentCount = 0;
			LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
			if (arguments == nullptr) {
				return {};
			}

			std::vector<std::wstring> mediaPaths;
			for (int index = 1; index < argumentCount; ++index) {
				if (arguments[index][0] != L'\0') {
					mediaPaths.emplace_back(arguments[index]);
				}
			}
			LocalFree(arguments);
			return mediaPaths;
		}

		bool ForwardMediaToRunningInstance(const std::vector<std::wstring>& mediaPaths) {
			HWND window = nullptr;
			for (int attempt = 0; attempt < 100 && window == nullptr; ++attempt) {
				window = FindWindowW(kControlClass, nullptr);
				if (window == nullptr) {
					Sleep(50);
				}
			}
			if (window == nullptr) {
				return false;
			}

			// This instance was started by the shell and may take the foreground;
			// the tray instance may not. Pass the right on so the MPC-HC window it
			// starts can come to the front instead of flashing in the taskbar.
			DWORD guardPid = 0;
			GetWindowThreadProcessId(window, &guardPid);
			if (guardPid != 0) {
				AllowSetForegroundWindow(guardPid);
			}

			bool allAccepted = true;
			for (const std::wstring& mediaPath : mediaPaths) {
				COPYDATASTRUCT data{};
				data.dwData = kOpenMediaCopyDataId;
				data.cbData = static_cast<DWORD>((mediaPath.size() + 1) * sizeof(wchar_t));
				data.lpData = const_cast<wchar_t*>(mediaPath.c_str());

				DWORD_PTR accepted = FALSE;
				if (SendMessageTimeoutW(window, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&data),
					SMTO_ABORTIFHUNG, kForwardTimeoutMs, &accepted) == 0 || accepted != TRUE) {
					allAccepted = false;
				}
			}
			return allAccepted;
		}

		void LoadApplicationIcons() {
			g_largeIcon = static_cast<HICON>(LoadImageW(
				g_instance,
				MAKEINTRESOURCEW(IDI_APP_ICON),
				IMAGE_ICON,
				GetSystemMetrics(SM_CXICON),
				GetSystemMetrics(SM_CYICON),
				LR_DEFAULTCOLOR | LR_SHARED));
			g_smallIcon = static_cast<HICON>(LoadImageW(
				g_instance,
				MAKEINTRESOURCEW(IDI_APP_ICON),
				IMAGE_ICON,
				GetSystemMetrics(SM_CXSMICON),
				GetSystemMetrics(SM_CYSMICON),
				LR_DEFAULTCOLOR | LR_SHARED));

			if (g_largeIcon == nullptr) {
				g_largeIcon = LoadIconW(nullptr, IDI_APPLICATION);
			}
			if (g_smallIcon == nullptr) {
				g_smallIcon = LoadIconW(nullptr, IDI_APPLICATION);
			}
		}

	} // namespace

	int RunApplication(HINSTANCE instance) {
		g_instance = instance;
		const std::vector<std::wstring> mediaPaths = GetMediaArguments();

		g_singleInstanceMutex = CreateMutexW(nullptr, FALSE, kMutexName);
		if (g_singleInstanceMutex == nullptr) {
			return 1;
		}

		if (GetLastError() == ERROR_ALREADY_EXISTS) {
			if (mediaPaths.empty()) {
				MessageBoxW(nullptr, L"CompositionRateGuard is already running.", kAppName, MB_OK);
			}
			else if (!ForwardMediaToRunningInstance(mediaPaths)) {
				MessageBoxW(
					nullptr,
					L"CompositionRateGuard is already starting, but the video could not be sent to it.",
					kAppName,
					MB_OK | MB_ICONERROR);
			}
			CloseHandle(g_singleInstanceMutex);
			g_singleInstanceMutex = nullptr;
			return 0;
		}

		// DPI awareness (PerMonitorV2) comes from app.manifest.
		LoadApplicationIcons();

		g_configDirectory = GetConfigDirectory();
		g_iniPath = g_configDirectory + L"\\guard.ini";
		InitializeLogging(g_configDirectory + L"\\guard.log");
		g_settings = LoadSettings(g_iniPath);

		Log(L"Application starting.");
		LogFormat(L"Configuration directory: %ls", g_configDirectory.c_str());
		g_taskbarCreatedMessage = RegisterWindowMessageW(L"TaskbarCreated");

		if (!RegisterWindowClasses() || !CreateApplicationWindows()) {
			const DWORD error = GetLastError();
			wchar_t message[256]{};
			_snwprintf_s(message, std::size(message), _TRUNCATE,
				L"Failed to initialize CompositionRateGuard. Win32 error: %lu", error);
			Log(message);
			MessageBoxW(nullptr, message, kAppName, MB_OK | MB_ICONERROR);
			CloseHandle(g_singleInstanceMutex);
			g_singleInstanceMutex = nullptr;
			return 1;
		}

		AddTrayIcon();
		PollPlayer();

		if (!mediaPaths.empty()) {
			bool launchedAny = false;
			for (const std::wstring& mediaPath : mediaPaths) {
				launchedAny = LaunchRequestedMedia(mediaPath) || launchedAny;
			}
			if (launchedAny) {
				// Only an instance originally launched for a video is session-scoped.
				// Videos forwarded later cannot change a persistent instance's lifetime.
				g_exitWhenPlayersClose = true;
			}
			else {
				PostMessageW(g_controlWindow, WM_CLOSE, 0, 0);
			}
		}

		MSG message{};
		while (GetMessageW(&message, nullptr, 0, 0) > 0) {
			TranslateMessage(&message);
			DispatchMessageW(&message);
		}

		Log(L"Application exiting.");
		g_overlay.Destroy();
		CloseHandle(g_singleInstanceMutex);
		g_singleInstanceMutex = nullptr;
		return static_cast<int>(message.wParam);
	}

} // namespace crg
