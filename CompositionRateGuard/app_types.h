#pragma once

#include <windows.h>

#include <string>

namespace crg {

	enum class OverlayMode : int {
		// Keep value 0 so existing installations automatically migrate from the
		// ineffective fully transparent mode to the near-invisible 1x1 mode.
		NearInvisible = 0,
		// Keep this distinct from values used by earlier releases so an old saved
		// mode always migrates to the safe default.
		VisibleTest = 10,
	};

	struct Settings {
		bool enabled = true;
		OverlayMode mode = OverlayMode::NearInvisible;
		std::wstring playerPath;
	};

	struct PlayerWindow {
		HWND hwnd = nullptr;
		DWORD pid = 0;
		RECT bounds{};
		RECT monitor{};
		long long area = 0;
		LONG_PTR style = 0;
		LONG_PTR exStyle = 0;
	};

} // namespace crg
