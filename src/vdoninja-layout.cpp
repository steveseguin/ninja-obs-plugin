/*
 * OBS VDO.Ninja Plugin
 * Layout helper utilities
 */

#include "vdoninja-layout.h"

#include <cmath>

namespace vdoninja
{

std::vector<LayoutRect> buildGridLayout(size_t itemCount, uint32_t canvasWidth, uint32_t canvasHeight)
{
	std::vector<LayoutRect> layout;
	if (itemCount == 0) {
		return layout;
	}

	const uint32_t safeWidth = canvasWidth == 0 ? 1 : canvasWidth;
	const uint32_t safeHeight = canvasHeight == 0 ? 1 : canvasHeight;

	const float colsFloat = std::ceil(std::sqrt(static_cast<float>(itemCount)));
	const uint32_t cols = colsFloat < 1.0f ? 1u : static_cast<uint32_t>(colsFloat);
	const uint32_t rows = static_cast<uint32_t>(std::ceil(static_cast<float>(itemCount) / static_cast<float>(cols)));

	const float cellWidth = static_cast<float>(safeWidth) / static_cast<float>(cols);
	const float cellHeight = static_cast<float>(safeHeight) / static_cast<float>(rows);

	layout.reserve(itemCount);
	for (size_t i = 0; i < itemCount; ++i) {
		const uint32_t row = static_cast<uint32_t>(i) / cols;
		const uint32_t col = static_cast<uint32_t>(i) % cols;

		LayoutRect rect;
		rect.x = cellWidth * static_cast<float>(col);
		rect.y = cellHeight * static_cast<float>(row);
		rect.width = cellWidth;
		rect.height = cellHeight;
		layout.push_back(rect);
	}

	return layout;
}

} // namespace vdoninja
