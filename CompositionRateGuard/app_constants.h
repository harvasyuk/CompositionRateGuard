#pragma once

#include <cstddef>

namespace crg {

	inline constexpr wchar_t kAppName[] = L"CompositionRateGuard";
	inline constexpr wchar_t kControlClass[] = L"CompositionRateGuard.Control";
	inline constexpr wchar_t kOverlayClass[] = L"CompositionRateGuard.Overlay";
	inline constexpr wchar_t kMutexName[] = L"Local\\CompositionRateGuard.SingleInstance";

	// Maximum Windows path length in characters, including long paths.
	inline constexpr std::size_t kLongPathCapacity = 32768;

} // namespace crg
