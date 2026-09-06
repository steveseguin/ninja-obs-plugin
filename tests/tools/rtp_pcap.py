"""Read visible RTP headers from synthetic TURN captures; SR bodies remain encrypted."""
import ipaddress
import struct


def turn_payload(data):
    if len(data) >= 4 and 0x40 <= data[0] <= 0x7f:
        size = int.from_bytes(data[2:4], 'big')
        return data[4:4 + size] if len(data) >= size + 4 else b''
    if len(data) >= 20 and data[4:8] == b'\x21\x12\xa4\x42':
        end = 20 + int.from_bytes(data[2:4], 'big')
        if end > len(data):
            return b''
        offset = 20
        while offset + 4 <= end:
            kind, size = struct.unpack_from('!HH', data, offset)
            offset += 4
            if offset + size > end:
                return b''
            if kind == 0x13:  # STUN DATA attribute
                return data[offset:offset + size]
            offset += (size + 3) & ~3
        return b''
    return data


def packet_record(data, linktype, epoch):
    offsets = {1: 14, 113: 16, 276: 20}
    if linktype not in offsets:
        raise ValueError(f'Unsupported pcap link type {linktype}')
    offset = offsets[linktype]
    if len(data) < offset + 20 or data[offset] >> 4 != 4:
        return None
    ip = data[offset:]
    size = int.from_bytes(ip[2:4], 'big')
    header = (ip[0] & 15) * 4
    if header < 20 or size > len(ip) or size < header + 8 or ip[9] != 17:
        return None
    if int.from_bytes(ip[6:8], 'big') & 0x3fff:  # Ignore fragmented datagrams.
        return None
    src = str(ipaddress.ip_address(ip[12:16]))
    dst = str(ipaddress.ip_address(ip[16:20]))
    sport, dport, udp_size = struct.unpack_from('!HHH', ip, header)
    if udp_size < 8 or header + udp_size > size:
        return None
    payload = turn_payload(ip[header + 8:header + udp_size])
    if len(payload) < 12 or payload[0] >> 6 != 2:
        return None
    # SRTCP encrypts the SR NTP/RTP mapping. Never parse ciphertext as timestamps.
    if 192 <= payload[1] <= 223:
        return None
    seq, timestamp, ssrc = struct.unpack_from('!HII', payload, 2)
    return {'epochMs': epoch * 1000, 'src': src, 'dst': dst, 'sport': sport, 'dport': dport,
            'sequence': seq, 'rtpTimestamp': timestamp, 'ssrc': ssrc,
            'marker': bool(payload[1] & 128), 'payloadType': payload[1] & 127, 'bytes': len(payload)}


def read_pcap(filename):
    with open(filename, 'rb') as file:
        header = file.read(24)
        formats = {b'\xd4\xc3\xb2\xa1': ('<', 1e6), b'\xa1\xb2\xc3\xd4': ('>', 1e6),
                   b'\x4d\x3c\xb2\xa1': ('<', 1e9), b'\xa1\xb2\x3c\x4d': ('>', 1e9)}
        if len(header) != 24 or header[:4] not in formats:
            raise ValueError('Expected classic pcap header')
        endian, scale = formats[header[:4]]
        linktype = struct.unpack_from(endian + 'I', header, 20)[0]
        while True:
            packet = file.read(16)
            if not packet:
                break
            if len(packet) != 16:
                raise ValueError('Truncated pcap packet header')
            seconds, fraction, captured, original = struct.unpack(endian + 'IIII', packet)
            if captured > 16 * 1024 * 1024:
                raise ValueError('Oversized pcap packet')
            data = file.read(captured)
            if len(data) != captured or captured != original:
                raise ValueError('Truncated captured packet')
            record = packet_record(data, linktype, seconds + fraction / scale)
            if record:
                yield record


if __name__ == '__main__':
    import argparse
    import json
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture')
    parser.add_argument('output')
    args = parser.parse_args()
    records = list(read_pcap(args.capture))
    with open(args.output, 'w') as file:
        json.dump(records, file)
    print(f'Extracted {len(records)} visible RTP headers')
