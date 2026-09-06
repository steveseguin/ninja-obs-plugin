"""Generate a diagnostic controller copy; keep the pinned upstream tree untouched."""
from pathlib import Path
import sys

source, destination, variant = sys.argv[1:]
text = Path(source).read_text()

def replace(before, after):
    global text
    if text.count(before) != 1:
        raise RuntimeError(f'Expected one controller source site: {before!r}')
    text = text.replace(before, after)

if variant not in ['all-arrivals', 'first-packet']:
    raise RuntimeError('Unknown sample policy')
replace('!metadata.delayed_by_retransmission && metadata.receive_time', 'metadata.receive_time')
if variant == 'first-packet':
    replace('struct FrameMetadata {', 'Timestamp MinReceiveTime(const EncodedFrame& frame);\n\nstruct FrameMetadata {')
    replace('receive_time(frame.ReceivedTimestamp()) {}',
            'receive_time(frame.ReceivedTimestamp()),\n        first_receive_time(MinReceiveTime(frame)) {}')
    replace('  const std::optional<Timestamp> receive_time;',
            '  const std::optional<Timestamp> receive_time;\n  const Timestamp first_receive_time;')
    replace('*metadata.receive_time);',
            'metadata.first_receive_time.IsFinite() ? metadata.first_receive_time : *metadata.receive_time);')
Path(destination).write_text(text)
