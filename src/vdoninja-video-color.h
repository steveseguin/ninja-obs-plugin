/* SPDX-License-Identifier: AGPL-3.0-only */
#pragma once

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

namespace vdoninja
{

// sws_scale receives planes, not AVFrame metadata. Apply this on every frame:
// a cached scaler can survive a matrix/range change without a size change.
inline bool configureDecodedVideoColor(SwsContext *context, const AVFrame *frame)
{
	int matrix = SWS_CS_DEFAULT; // Preserve the legacy BT.601 fallback for untagged SDR.
	switch (frame->colorspace) {
	case AVCOL_SPC_BT709:
		matrix = SWS_CS_ITU709;
		break;
	case AVCOL_SPC_FCC:
		matrix = SWS_CS_FCC;
		break;
	case AVCOL_SPC_BT470BG:
	case AVCOL_SPC_SMPTE170M:
		matrix = SWS_CS_ITU601;
		break;
	case AVCOL_SPC_SMPTE240M:
		matrix = SWS_CS_SMPTE240M;
		break;
	case AVCOL_SPC_BT2020_NCL:
		matrix = SWS_CS_BT2020;
		break;
	default:
		break;
	}
	const auto format = static_cast<AVPixelFormat>(frame->format);
	const auto *descriptor = av_pix_fmt_desc_get(format);
	const bool fullRange = frame->color_range == AVCOL_RANGE_JPEG ||
	                       (descriptor && (descriptor->flags & AV_PIX_FMT_FLAG_RGB)) ||
	                       (frame->color_range == AVCOL_RANGE_UNSPECIFIED &&
	                        (format == AV_PIX_FMT_YUVJ420P || format == AV_PIX_FMT_YUVJ422P ||
	                         format == AV_PIX_FMT_YUVJ444P || format == AV_PIX_FMT_YUVJ440P));
	const int *coefficients = sws_getCoefficients(matrix);
	return sws_setColorspaceDetails(context, coefficients, fullRange ? 1 : 0, coefficients, 1, 0, 1 << 16, 1 << 16) >=
	       0;
}

} // namespace vdoninja
