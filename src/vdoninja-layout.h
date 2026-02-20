/*
 * OBS VDO.Ninja Plugin
 * Layout helper utilities
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace vdoninja
{

struct LayoutRect {
	float x = 0.0f;
	float y = 0.0f;
	float width = 0.0f;
	float height = 0.0f;
};

std::vector<LayoutRect> buildGridLayout(size_t itemCount, uint32_t canvasWidth, uint32_t canvasHeight);

} // namespace vdoninja
