#pragma once

#include "app_types.h"

#include <windows.h>

#include <string>

namespace crg {

	bool LaunchMediaFile(
		HWND owner,
		const std::wstring& mediaPath,
		Settings& settings,
		const std::wstring& iniPath);

	bool IsAnyMpcHcProcessRunning();
	PlayerWindow FindPlayerWindow(HWND overlayWindow);
	bool IsBorderlessFullscreenWindow(const PlayerWindow& player);
	void LogPlayerState(const PlayerWindow& player, const wchar_t* state);

} // namespace crg
