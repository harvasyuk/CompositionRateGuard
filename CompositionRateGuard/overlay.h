#pragma once

#include "app_types.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

namespace crg {

	class Overlay {
	public:
		Overlay() = default;
		Overlay(const Overlay&) = delete;
		Overlay& operator=(const Overlay&) = delete;
		~Overlay();

		bool RegisterWindowClass(HINSTANCE instance);
		bool Create(HINSTANCE instance, OverlayMode mode);
		void Destroy();

		void SetMode(OverlayMode mode);
		void ShowFor(const PlayerWindow& player);
		void Hide();

		[[nodiscard]] HWND Window() const { return window_; }
		[[nodiscard]] bool IsVisible() const { return visible_; }

	private:
		static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
		LRESULT HandleMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

		[[nodiscard]] BYTE Alpha() const;
		[[nodiscard]] RECT CalculateRect(const RECT& monitor) const;

		// Render thread: repaints the surface once per DWM composition pass, so the
		// UI thread never blocks in DwmFlush.
		bool StartRenderThread();
		void StopRenderThread();
		void RenderLoop(HWND window);
		void RenderFrame(HWND window);
		void PaintVisibleGradient(HWND window, HDC dc);

		HWND window_ = nullptr;
		std::atomic<OverlayMode> mode_{ OverlayMode::NearInvisible };
		bool visible_ = false;
		HWND currentPlayer_ = nullptr;
		RECT lastRect_{};

		std::thread renderThread_;
		// Manual-reset event: signaled while the overlay is visible or stopping.
		HANDLE renderEvent_ = nullptr;
		std::atomic<bool> stopRendering_{ false };

		// Owned by the render thread.
		unsigned int animationPhase_ = 0;
		std::vector<std::uint32_t> gradientPixels_;
	};

} // namespace crg
