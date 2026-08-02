#pragma once

#include "app_types.h"

#include <string>

namespace crg {

	std::wstring GetConfigDirectory();
	Settings LoadSettings(const std::wstring& iniPath);
	void SaveSettings(const std::wstring& iniPath, const Settings& settings);

	bool IsStartupEnabled();
	bool SetStartupEnabled(bool enabled);

} // namespace crg
