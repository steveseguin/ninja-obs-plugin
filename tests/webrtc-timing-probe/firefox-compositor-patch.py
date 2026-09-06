"""Trace Linux Firefox image enqueue and actual first-composition notifications.

Enable MOZ_LOG=VdoninjaCompositor:3. This adds no scheduling or buffer changes.
"""
from pathlib import Path
import sys

root = Path(sys.argv[1]).resolve()
if (root / 'browser/config/version.txt').read_text().strip() != '146.0.1' or \
        '86bb7f6af6312ba3c0161085f854bcdff68f1a91' not in (root / 'sourcestamp.txt').read_text():
    raise RuntimeError('Expected Firefox 146.0.1 release source')
path = root / 'gfx/layers/ImageContainer.cpp'
text = path.read_text()
replacements = [
    ('#include "ImageContainer.h"', '#include "ImageContainer.h"\n#include "mozilla/Logging.h"'),
    ('namespace mozilla::layers {', '''namespace mozilla::layers {
static LazyLogModule gVdoninjaCompositor("VdoninjaCompositor");'''),
    ('    img->mProducerID = aImages[i].mProducerID;', '''    img->mProducerID = aImages[i].mProducerID;
#ifdef XP_LINUX
    if (img->mRtpTimestamp) {
      MOZ_LOG(gVdoninjaCompositor, LogLevel::Info,
              ("VDONINJA_FF_COMPOSITOR event=enqueue object=%p producer=%u frame=%u rtp=%u now_us=%llu reference_us=%llu",
               this, img->mProducerID, img->mFrameID, *img->mRtpTimestamp,
               static_cast<unsigned long long>(TimeStamp::Now().RawClockMonotonicNanosecondsSinceBoot() / 1000),
               static_cast<unsigned long long>(img->mTimeStamp.RawClockMonotonicNanosecondsSinceBoot() / 1000)));
    }
#endif'''),
    ('  ++mPaintCount;', '''  ++mPaintCount;
#ifdef XP_LINUX
  MOZ_LOG(gVdoninjaCompositor, LogLevel::Info,
          ("VDONINJA_FF_COMPOSITOR event=composite object=%p producer=%u frame=%u now_us=%llu composite_us=%llu reference_us=%llu paint_count=%u",
           this, aNotification.producerID(), aNotification.frameID(),
           static_cast<unsigned long long>(TimeStamp::Now().RawClockMonotonicNanosecondsSinceBoot() / 1000),
           static_cast<unsigned long long>(aNotification.firstCompositeTimeStamp().RawClockMonotonicNanosecondsSinceBoot() / 1000),
           static_cast<unsigned long long>(aNotification.imageTimeStamp().RawClockMonotonicNanosecondsSinceBoot() / 1000),
           mPaintCount));
#endif'''),
]
for before, after in replacements:
    if text.count(before) != 1:
        raise RuntimeError(f'Expected one unpatched source site: {before!r}')
    text = text.replace(before, after)
path.write_text(text)
print('Applied opt-in Linux Firefox compositor trace')
