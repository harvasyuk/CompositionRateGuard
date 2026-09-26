#include "settings.h"

#include "app_constants.h"

#include <windows.h>

#include <cstdio>
#include <iterator>
#include <vector>

namespace crg {
	namespace {

		constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
		constexpr wchar_t kRunValueName[] = L"CompositionRateGuard";

		std::wstring GetExecutablePath() {
			std::vector<wchar_t> path(kLongPathCapacity);
			const DWORD length = GetModuleFileNameW(
				nullptr, path.data(), static_cast<DWORD>(path.size()));
			if (length == 0 || length >= path.size()) {
				return {};
			}
			return std::wstring(path.data(), length);
		}

		// The Run value this executable registers; empty if its path is unknown.
		std::wstring GetStartupCommand() {
			const std::wstring executablePath = GetExecutablePath();
			return executablePath.empty() ? std::wstring() : L"\"" + executablePath + L"\"";
		}

	} // namespace

	std::wstring GetConfigDirectory() {
		std::vector<wchar_t> buffer(kLongPathCapacity);
		const DWORD length = GetEnvironmentVariableW(
			L"LOCALAPPDATA", buffer.data(), static_cast<DWORD>(buffer.size()));

		std::wstring directory;
		if (length > 0 && length < buffer.size()) {
			directory.assign(buffer.data(), length);
		}
		else {
			wchar_t temp[MAX_PATH]{};
			const DWORD tempLength = GetTempPathW(MAX_PATH, temp);
			directory.assign(temp, tempLength);
		}

		if (!directory.empty() && directory.back() != L'\\') {
			directory.push_back(L'\\');
		}

		const std::wstring configDirectory = directory + L"CompositionRateGuard";
		CreateDirectoryW(configDirectory.c_str(), nullptr);
		return configDirectory;
	}

	Settings LoadSettings(const std::wstring& iniPath) {
		Settings settings;
		settings.enabled = GetPrivateProfileIntW(L"Guard", L"Enabled", 1, iniPath.c_str()) != 0;

		const int mode = GetPrivateProfileIntW(
			L"Guard", L"Mode", static_cast<int>(OverlayMode::NearInvisible), iniPath.c_str());
		settings.mode = mode == static_cast<int>(OverlayMode::VisibleTest)
			? OverlayMode::VisibleTest
			: OverlayMode::NearInvisible;

		std::vector<wchar_t> playerPath(kLongPathCapacity);
		GetPrivateProfileStringW(
			L"Player", L"Executable", L"", playerPath.data(),
			static_cast<DWORD>(playerPath.size()), iniPath.c_str());
		settings.playerPath.assign(playerPath.data());
		return settings;
	}

	void SaveSettings(const std::wstring& iniPath, const Settings& settings) {
		WritePrivateProfileStringW(
			L"Guard", L"Enabled", settings.enabled ? L"1" : L"0", iniPath.c_str());

		wchar_t mode[16]{};
		_snwprintf_s(mode, std::size(mode), _TRUNCATE, L"%d", static_cast<int>(settings.mode));
		WritePrivateProfileStringW(L"Guard", L"Mode", mode, iniPath.c_str());
		WritePrivateProfileStringW(
			L"Player", L"Executable", settings.playerPath.c_str(), iniPath.c_str());
	}

	bool IsStartupEnabled() {
		std::vector<wchar_t> command(kLongPathCapacity);
		DWORD size = static_cast<DWORD>(command.size() * sizeof(wchar_t));
		if (RegGetValueW(
			HKEY_CURRENT_USER,
			kRunKey,
			kRunValueName,
			RRF_RT_REG_SZ,
			nullptr,
			command.data(),
			&size) != ERROR_SUCCESS) {
			return false;
		}

		// An entry left behind by a moved or renamed executable does not count, so
		// enabling the option again rewrites it with the current path.
		const std::wstring expected = GetStartupCommand();
		return !expected.empty() &&
			CompareStringOrdinal(command.data(), -1, expected.c_str(), -1, TRUE) == CSTR_EQUAL;
	}

	bool SetStartupEnabled(bool enabled) {
		HKEY key = nullptr;
		const LSTATUS openResult = RegCreateKeyExW(
			HKEY_CURRENT_USER,
			kRunKey,
			0,
			nullptr,
			REG_OPTION_NON_VOLATILE,
			KEY_SET_VALUE,
			nullptr,
			&key,
			nullptr);
		if (openResult != ERROR_SUCCESS) {
			return false;
		}

		LSTATUS result = ERROR_SUCCESS;
		if (enabled) {
			const std::wstring command = GetStartupCommand();
			if (command.empty()) {
				RegCloseKey(key);
				return false;
			}

			result = RegSetValueExW(
				key,
				kRunValueName,
				0,
				REG_SZ,
				reinterpret_cast<const BYTE*>(command.c_str()),
				static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
		}
		else {
			result = RegDeleteValueW(key, kRunValueName);
			if (result == ERROR_FILE_NOT_FOUND) {
				result = ERROR_SUCCESS;
			}
		}

		RegCloseKey(key);
		return result == ERROR_SUCCESS;
	}

} // namespace crg
