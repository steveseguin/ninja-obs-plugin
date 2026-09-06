import importlib.util
from pathlib import Path
import struct
import unittest
spec = importlib.util.spec_from_file_location('rtp_pcap', Path(__file__).parent / 'tools/rtp_pcap.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)

class RtpCaptureTest(unittest.TestCase):
    def packet(self, payload):
        udp = struct.pack('!HHHH', 45678, 3478, 8 + len(payload), 0) + payload
        ip = bytes.fromhex('4500') + struct.pack('!H', 20 + len(udp)) + bytes.fromhex('0000000040110000ac110001ac110002') + udp
        return b'\0' * 14 + ip

    def test_channel_data_preserves_visible_rtp_fields(self):
        rtp = bytes.fromhex('80e0ffffffffff0012345678') + b'encrypted'
        packet = self.packet(struct.pack('!HH', 0x4001, len(rtp)) + rtp)
        result = module.packet_record(packet, 1, 123.5)
        self.assertEqual(result['sequence'], 65535)
        self.assertEqual(result['rtpTimestamp'], 0xffffff00)
        self.assertEqual(result['ssrc'], 0x12345678)
        self.assertEqual(result['epochMs'], 123500)
        self.assertTrue(result['marker'])

    def test_stun_data_attribute_ignores_padding(self):
        rtp = bytes.fromhex('8060000100000bb800000001') + b'x'
        attributes = struct.pack('!HH', 0x13, len(rtp)) + rtp + b'\0' * 3
        packet = struct.pack('!HHI', 0x17, len(attributes), 0x2112a442) + b'\0' * 12 + attributes
        self.assertEqual(module.turn_payload(packet), rtp)

    def test_encrypted_sender_report_is_not_a_plaintext_mapping(self):
        self.assertIsNone(module.packet_record(self.packet(bytes.fromhex('80c80006') + b'\0' * 24), 1, 0))

    def test_truncated_channel_data_and_ip_are_rejected(self):
        self.assertEqual(module.turn_payload(bytes.fromhex('40010020') + b'x'), b'')
        self.assertIsNone(module.packet_record(self.packet(b'\x80\x60' + b'\0' * 10)[:-1], 1, 0))

if __name__ == '__main__':
    unittest.main()
