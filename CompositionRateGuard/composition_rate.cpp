#include "composition_rate.h"

#include <windows.h>
#include <dwmapi.h>

#include <cstdio>

namespace crg {
	namespace {

		double RatioToHz(unsigned int numerator, unsigned int denominator) {
			return denominator != 0
				? static_cast<double>(numerator) / static_cast<double>(denominator)
				: 0.0;
		}

	} // namespace

	double CompositionRate::ComposeHz() const {
		return RatioToHz(composeNumerator, composeDenominator);
	}

	double CompositionRate::RefreshHz() const {
		return RatioToHz(refreshNumerator, refreshDenominator);
	}

	bool CompositionRate::SameAs(const CompositionRate& other) const {
		if (valid != other.valid) {
			return false;
		}
		// Compare ratios exactly so equivalent fractions such as 48000/1001 and
		// 96000/2002 are not reported as a change.
		return static_cast<unsigned long long>(composeNumerator) * other.composeDenominator ==
			static_cast<unsigned long long>(other.composeNumerator) * composeDenominator &&
			static_cast<unsigned long long>(refreshNumerator) * other.refreshDenominator ==
			static_cast<unsigned long long>(other.refreshNumerator) * refreshDenominator;
	}

	CompositionRate QueryCompositionRate() {
		DWM_TIMING_INFO info{};
		info.cbSize = sizeof(info);

		CompositionRate rate;
		if (FAILED(DwmGetCompositionTimingInfo(nullptr, &info)) ||
			info.rateCompose.uiDenominator == 0 ||
			info.rateRefresh.uiDenominator == 0) {
			return rate;
		}

		rate.valid = true;
		rate.composeNumerator = info.rateCompose.uiNumerator;
		rate.composeDenominator = info.rateCompose.uiDenominator;
		rate.refreshNumerator = info.rateRefresh.uiNumerator;
		rate.refreshDenominator = info.rateRefresh.uiDenominator;
		return rate;
	}

	void FormatCompositionRate(const CompositionRate& rate, wchar_t* buffer, size_t capacity) {
		if (!rate.valid) {
			_snwprintf_s(buffer, capacity, _TRUNCATE, L"Composition rate: unavailable");
			return;
		}
		_snwprintf_s(buffer, capacity, _TRUNCATE,
			L"Composition rate: %.3f Hz (refresh %.3f Hz)",
			rate.ComposeHz(), rate.RefreshHz());
	}

} // namespace crg
