#pragma once
/* Pure DPR helper for the Sailfish OS target.
 * Extracted into a header so host unit tests can verify device-density math
 * without initializing SDL2 or the full UI tree. */

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace gea::sailfish {

// CSS width an "auto" DPR keeps at least: the common phone layout width.
inline constexpr int kAutoCssShortSide = 360;

// Parses GEA_SAILFISH_DPR, falling back to defaultDpr ("0" means auto).
inline double parseEnvDpr(const char *envVal, double defaultDpr)
{
	if (envVal && *envVal) {
		if (std::strcmp(envVal, "auto") == 0) return 0.0;
		const double dpr = std::atof(envVal);
		if (dpr >= 0.5 && dpr <= 8.0) return dpr;
	}
	return defaultDpr;
}

// Computes the DPR handed to the framework.
// Explicit app DPR (> 0) is multiplied by Wayland outputRatio (canvas is in
// physical pixels). Auto (<= 0) picks the largest integer that keeps the
// panel's short side (divided by windowScale) at least 360 CSS pixels wide.
inline double effectiveDevicePixelRatio(double appDpr, double outputRatio, int windowScale, int panelShortSide)
{
	if (appDpr > 0.0) return appDpr * outputRatio;
	const int scale = std::max(1, windowScale);
	const int shortSide = panelShortSide / scale;
	return static_cast<double>(std::max(1, shortSide / kAutoCssShortSide));
}

}  // namespace gea::sailfish
