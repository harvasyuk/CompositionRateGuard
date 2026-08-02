#include "overlay.h"

#include "app_constants.h"
#include "logging.h"

#include <dwmapi.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <iterator>

namespace crg {
	namespace {

		bool RectsEqual(const RECT& a, const RECT& b) {
			return a.left == b.left && a.top == b.top &&
				a.right == b.right && a.bottom == b.bottom;
		}

	} // namespace

	bool Overlay::RegisterWindowClass(HINSTANCE instance) {
		WNDCLASSEXW windowClass{};
		windowClass.cbSize = sizeof(windowClass);
		windowClass.hInstance = instance;
		windowClass.lpfnWndProc = WindowProc;
		windowClass.lpszClassName = kOverlayClass;
		windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
		windowClass.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
		return RegisterClassExW(&windowClass) != 0;
	}

	bool Overlay::Create(HINSTANCE instance, OverlayMode mode) {
		mode_ = mode;
		constexpr DWORD extendedStyles =
			WS_EX_LAYERED |
			WS_EX_TRANSPARENT |
			WS_EX_NOACTIVATE |
			WS_EX_TOOLWINDOW;

		window_ = CreateWindowExW(
			extendedStyles,
			kOverlayClass,
			L"CompositionRateGuard Overlay",
			WS_POPUP,
			0, 0, 1, 1,
			nullptr,
			nullptr,
			instance,
			this);
		if (window_ == nullptr) {
			return false;
		}

		// Alpha 0 can be optimized away by DWM and does not reliably keep the
		// desired composition rate.
		return SetLayeredWindowAttributes(window_, 0, Alpha(), LWA_ALPHA) != FALSE;
	}

	void Overlay::Destroy() {
		if (window_ != nullptr) {
			DestroyWindow(window_);
			window_ = nullptr;
		}
		visible_ = false;
		currentPlayer_ = nullptr;
		currentPlayerPid_ = 0;
	}

	void Overlay::SetMode(OverlayMode mode) {
		mode_ = mode;
		lastRect_ = RECT{};
		if (window_ != nullptr) {
			SetLayeredWindowAttributes(window_, 0, Alpha(), LWA_ALPHA);
		}
	}

	BYTE Overlay::Alpha() const {
		// Alpha 1 is the least opaque value that is not fully transparent.
		return mode_ == OverlayMode::VisibleTest ? 255 : 1;
	}

	RECT Overlay::CalculateRect(const RECT& monitor) const {
		RECT result = monitor;
		if (mode_ == OverlayMode::NearInvisible) {
			result.left = std::max(monitor.left, monitor.right - 1);
			result.top = std::max(monitor.top, monitor.bottom - 1);
		}
		else {
			result.left = std::max(monitor.left, monitor.right - 200);
			result.top = std::max(monitor.top, monitor.bottom - 40);
		}
		return result;
	}

	void Overlay::ShowFor(const PlayerWindow& player) {
		if (window_ == nullptr) {
			return;
		}

		const RECT overlayRect = CalculateRect(player.monitor);
		const int width = overlayRect.right - overlayRect.left;
		const int height = overlayRect.bottom - overlayRect.top;
		const bool playerChanged = player.hwnd != currentPlayer_;
		const bool rectChanged = !RectsEqual(overlayRect, lastRect_);

		if (!visible_ || playerChanged || rectChanged) {
			SetWindowPos(
				window_,
				HWND_TOPMOST,
				overlayRect.left,
				overlayRect.top,
				width,
				height,
				SWP_NOACTIVATE | SWP_SHOWWINDOW);

			SetLayeredWindowAttributes(window_, 0, Alpha(), LWA_ALPHA);
			InvalidateRect(window_, nullptr, TRUE);
			UpdateWindow(window_);

			visible_ = true;
			currentPlayer_ = player.hwnd;
			currentPlayerPid_ = player.pid;
			lastRect_ = overlayRect;

			wchar_t message[512]{};
			_snwprintf_s(
				message, std::size(message), _TRUNCATE,
				L"Overlay shown: mode=%d, rect=(%ld,%ld)-(%ld,%ld), player PID=%lu.",
				static_cast<int>(mode_),
				overlayRect.left, overlayRect.top,
				overlayRect.right, overlayRect.bottom,
				player.pid);
			Log(message);
		}
		else {
			SetWindowPos(
				window_,
				HWND_TOPMOST,
				0, 0, 0, 0,
				SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
		}
	}

	void Overlay::Hide() {
		if (window_ != nullptr && visible_) {
			ShowWindow(window_, SW_HIDE);
			visible_ = false;
			Log(L"Overlay hidden.");
		}
		currentPlayer_ = nullptr;
		currentPlayerPid_ = 0;
	}

	void Overlay::Animate() {
		if (!visible_ || window_ == nullptr) {
			return;
		}

		++animationPhase_;
		RedrawWindow(
			window_,
			nullptr,
			nullptr,
			RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW | RDW_ALLCHILDREN);
		DwmFlush();
	}

	void Overlay::PaintVisibleGradient(HDC dc) {
		RECT client{};
		GetClientRect(window_, &client);
		const int width = client.right - client.left;
		const int height = client.bottom - client.top;
		if (width <= 0 || height <= 0) {
			return;
		}

		gradientPixels_.resize(static_cast<size_t>(width) * static_cast<size_t>(height));

		// Move a cyclic gradient across the rectangle. The periodic color formula
		// has matching left/right edges, so each cycle wraps without a jump.
		constexpr double pi = 3.14159265358979323846;
		constexpr unsigned int cycleFrames = 120;
		const double phase =
			static_cast<double>(animationPhase_ % cycleFrames) * 2.0 * pi /
			static_cast<double>(cycleFrames);

		const auto channel = [](double value) {
			return static_cast<std::uint32_t>(std::clamp(value, 0.0, 255.0) + 0.5);
			};

		for (int x = 0; x < width; ++x) {
			const double position = width > 1
				? static_cast<double>(x) / static_cast<double>(width - 1)
				: 0.0;
			const double angle = 2.0 * pi * position - phase;
			const double primaryWave = 0.5 + 0.5 * std::sin(angle);
			const double secondaryWave = 0.5 + 0.5 * std::sin(2.0 * angle + 0.8);
			const double movingHighlight = 0.82 + 0.18 * secondaryWave;
			const double baseR = (35.0 + 145.0 * primaryWave) * movingHighlight;
			const double baseG = (55.0 + 45.0 * secondaryWave) * movingHighlight;
			const double baseB = (225.0 - 65.0 * primaryWave) * movingHighlight;

			for (int y = 0; y < height; ++y) {
				const double vertical = height > 1
					? 1.0 - 0.15 * static_cast<double>(y) / static_cast<double>(height - 1)
					: 1.0;
				const std::uint32_t r = channel(baseR * vertical);
				const std::uint32_t g = channel(baseG * vertical);
				const std::uint32_t b = channel(baseB * vertical);

				// A 32-bit BI_RGB DIB uses little-endian B, G, R, unused bytes.
				gradientPixels_[static_cast<size_t>(y) * static_cast<size_t>(width) +
					static_cast<size_t>(x)] =
					(r << 16) | (g << 8) | b;
			}
		}

		BITMAPINFO bitmap{};
		bitmap.bmiHeader.biSize = sizeof(bitmap.bmiHeader);
		bitmap.bmiHeader.biWidth = width;
		bitmap.bmiHeader.biHeight = -height;
		bitmap.bmiHeader.biPlanes = 1;
		bitmap.bmiHeader.biBitCount = 32;
		bitmap.bmiHeader.biCompression = BI_RGB;

		SetDIBitsToDevice(
			dc,
			0, 0,
			static_cast<DWORD>(width), static_cast<DWORD>(height),
			0, 0,
			0, static_cast<UINT>(height),
			gradientPixels_.data(),
			&bitmap,
			DIB_RGB_COLORS);
	}

	LRESULT CALLBACK Overlay::WindowProc(
		HWND hwnd,
		UINT message,
		WPARAM wParam,
		LPARAM lParam) {
		Overlay* overlay = reinterpret_cast<Overlay*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
		if (message == WM_NCCREATE) {
			const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
			overlay = static_cast<Overlay*>(create->lpCreateParams);
			SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(overlay));
			if (overlay != nullptr) {
				overlay->window_ = hwnd;
			}
		}

		return overlay != nullptr
			? overlay->HandleMessage(hwnd, message, wParam, lParam)
			: DefWindowProcW(hwnd, message, wParam, lParam);
	}

	LRESULT Overlay::HandleMessage(
		HWND hwnd,
		UINT message,
		WPARAM wParam,
		LPARAM lParam) {
		switch (message) {
		case WM_NCHITTEST:
			return HTTRANSPARENT;
		case WM_MOUSEACTIVATE:
			return MA_NOACTIVATE;
		case WM_ERASEBKGND:
			return 1;
		case WM_PAINT: {
			PAINTSTRUCT paint{};
			HDC dc = BeginPaint(hwnd, &paint);

			if (mode_ == OverlayMode::VisibleTest) {
				PaintVisibleGradient(dc);
			}
			else {
				// The near-invisible pixel still changes every frame so DWM sees
				// genuine surface updates rather than a static layered window.
				const BYTE r = static_cast<BYTE>(64u + ((animationPhase_ * 37u) & 0xBFu));
				const BYTE g = static_cast<BYTE>((animationPhase_ * 73u) & 0x3Fu);
				const BYTE b = static_cast<BYTE>(64u + ((animationPhase_ * 109u) & 0xBFu));
				SetPixelV(dc, 0, 0, RGB(r, g, b));
			}
			EndPaint(hwnd, &paint);
			return 0;
		}
		case WM_NCDESTROY: {
			const LRESULT result = DefWindowProcW(hwnd, message, wParam, lParam);
			SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
			window_ = nullptr;
			return result;
		}
		default:
			return DefWindowProcW(hwnd, message, wParam, lParam);
		}
	}

} // namespace crg
