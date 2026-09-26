#pragma once

#include <cstddef>

namespace crg {

	struct CompositionRate {
		bool valid = false;
		unsigned int composeNumerator = 0;
		unsigned int composeDenominator = 0;
		unsigned int refreshNumerator = 0;
		unsigned int refreshDenominator = 0;

		[[nodiscard]] double ComposeHz() const;
		[[nodiscard]] double RefreshHz() const;
		[[nodiscard]] bool SameAs(const CompositionRate& other) const;
	};

	// DWM reports timing for the primary display only; since Windows 8.1 a
	// per-window query is no longer supported.
	CompositionRate QueryCompositionRate();
	void FormatCompositionRate(const CompositionRate& rate, wchar_t* buffer, size_t capacity);

} // namespace crg
