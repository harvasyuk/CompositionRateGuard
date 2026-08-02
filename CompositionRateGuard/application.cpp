#include "application.h"

#include "app_constants.h"
#include "app_types.h"
#include "logging.h"
#include "mpc_player.h"
#include "overlay.h"
#include "resource.h"
#include "settings.h"

#include <shellapi.h>

#include <cstdio>
#include <iterator>
#include <string>
#include <vector>

namespace crg {
	namespace {

		constexpr UINT kTrayCallback = WM_APP + 1;
		constexpr ULONG_PTR kOpenMediaCopyDataId = 0x4352474D; // "CRGM"
		constexpr UINT_PTR kPollTimerId = 1;
		constexpr UINT_PTR kAnimationTimerId = 2;
		constexpr UINT kPollIntervalMs = 1000;
		constexpr UINT kAnimationIntervalMs = 16;

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
			if (!LaunchMediaFile(g_controlWindow, mediaPath, g_settings, g_iniPath)) {
				return false;
			}

			// Refresh the startup grace period for association-scoped instances.
			// A persistent instance ignores this timestamp but can still launch media.
			g_lastPlayerLaunchTick = GetTickCount64();
			return true;
		}

		void HideOverlay() {
			const bool wasVisible = g_overlay.IsVisible();
			g_overlay.Hide();
			if (wasVisible && g_controlWindow != nullptr) {
				KillTimer(g_controlWindow, kAnimationTimerId);
			}
		}

		void ShowOverlayFor(const PlayerWindow& player) {
			const bool wasVisible = g_overlay.IsVisible();
			g_overlay.ShowFor(player);
			if (!wasVisible && g_overlay.IsVisible() && g_controlWindow != nullptr) {
				SetTimer(g_controlWindow, kAnimationTimerId, kAnimationIntervalMs, nullptr);
			}
		}

		void PollPlayer() {
			if (g_exitWhenPlayersClose &&
				GetTickCount64() - g_lastPlayerLaunchTick >= 2000 &&
				!IsAnyMpcHcProcessRunning()) {
				Log(L"No MPC-HC process remains; exiting association-launched guard.");
				DestroyWindow(g_controlWindow);
				return;
			}

			if (!g_settings.enabled) {
				SetDetectionState(DetectionState::Disabled, PlayerWindow{});
				HideOverlay();
				return;
			}

			const PlayerWindow player = FindPlayerWindow(g_overlay.Window());
			if (player.hwnd == nullptr) {
				SetDetectionState(DetectionState::NoPlayerWindow, player);
				HideOverlay();
				return;
			}

			if (!IsBorderlessFullscreenWindow(player)) {
				SetDetectionState(DetectionState::PlayerWindowed, player);
				HideOverlay();
				return;
			}

			// Tray interactions and utility windows must not momentarily tear down the
			// guard, so MPC-HC does not need to remain the foreground process.
			SetDetectionState(DetectionState::BorderlessFullscreen, player);
			ShowOverlayFor(player);
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

			AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, CurrentStatusText());
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
				return LaunchRequestedMedia(path) ? TRUE : FALSE;
			}
			case WM_CREATE:
				SetTimer(hwnd, kPollTimerId, kPollIntervalMs, nullptr);
				return 0;
			case WM_TIMER:
				if (wParam == kPollTimerId) {
					PollPlayer();
				}
				else if (wParam == kAnimationTimerId) {
					g_overlay.Animate();
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
				KillTimer(hwnd, kAnimationTimerId);
				HideOverlay();
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

			for (const std::wstring& mediaPath : mediaPaths) {
				COPYDATASTRUCT data{};
				data.dwData = kOpenMediaCopyDataId;
				data.cbData = static_cast<DWORD>((mediaPath.size() + 1) * sizeof(wchar_t));
				data.lpData = const_cast<wchar_t*>(mediaPath.c_str());
				SendMessageW(window, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&data));
			}
			return true;
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

		SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
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
			MessageBoxW(nullptr, message, kAppName, MB_OK | MB_ICONERROR);
			Log(message);
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
