#pragma once

#include "app_types.h"

#include <windows.h>

#include <cstdint>
#include <vector>

namespace crg {

	class Overlay {
	public:
		bool RegisterWindowClass(HINSTANCE instance);
		bool Create(HINSTANCE instance, OverlayMode mode);
		void Destroy();

		void SetMode(OverlayMode mode);
		void ShowFor(const PlayerWindow& player);
		void Hide();
		void Animate();

		[[nodiscard]] HWND Window() const { return window_; }
		[[nodiscard]] bool IsVisible() const { return visible_; }

	private:
		static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
		LRESULT HandleMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

		[[nodiscard]] BYTE Alpha() const;
		[[nodiscard]] RECT CalculateRect(const RECT& monitor) const;
		void PaintVisibleGradient(HDC dc);

		HWND window_ = nullptr;
		OverlayMode mode_ = OverlayMode::NearInvisible;
		bool visible_ = false;
		HWND currentPlayer_ = nullptr;
		DWORD currentPlayerPid_ = 0;
		RECT lastRect_{};
		unsigned int animationPhase_ = 0;
		std::vector<std::uint32_t> gradientPixels_;
	};

} // namespace crg
