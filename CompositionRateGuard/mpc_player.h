#pragma once

#include "app_types.h"

#include <windows.h>

#include <string>

namespace crg {

	// Resolves the player executable (asking the user if none is found), stores it
	// in playerPath, and starts it with mediaPath.
	bool LaunchMediaFile(HWND owner, const std::wstring& mediaPath, std::wstring& playerPath);

	// Players are recognized by an mpc-hc*.exe name or by the configured
	// executable's file name.
	bool IsAnyPlayerProcessRunning(const std::wstring& playerPath);
	PlayerWindow FindPlayerWindow(HWND overlayWindow, const std::wstring& playerPath);
	bool IsBorderlessFullscreenWindow(const PlayerWindow& player);
	void LogPlayerState(const PlayerWindow& player, const wchar_t* state);

} // namespace crg
