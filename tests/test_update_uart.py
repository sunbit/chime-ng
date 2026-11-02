#!/usr/bin/env python3
"""Fault injection for UART staging into the inactive slot; no device access."""
import hashlib
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import Mock
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import provision as p
import update_uart as u
from test_provision import Device


def image(build):
    payload = struct.pack('<II', 0x20040000, 0x20010009) + bytes([build]) * 1016
    return struct.pack('<8s6I32s', b'CHIMNG01', 0x43483332, 1, 0x20010000,
                       len(payload), build, 0, hashlib.sha256(payload).digest()) + payload


def record(active=0, pending=255, tried=0, generation=1):
    data = struct.pack('<I4BI', 0x43484231, active, pending, tried, 0, generation)
    return data + struct.pack('<I', zlib.crc32(data))


class UpdateDevice(Device):
    activation = 'ok'
    activations = 0

    def update_boot_record(self, data):
        self.activations += 1
        if self.activation != 'unchanged':
            self.files[u.BOOT_PATH] = data
        if self.activation != 'ok':
            raise p.LinkError('Lost activation ACK')


class UpdateTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.package = self.root / 'next.chime'
        self.package.write_bytes(image(2))
        self.loader = b'known-loader'
        self.allowed = {hashlib.sha256(self.loader).hexdigest()}
        self.files = {'/sys/mcuimg.bin': self.loader, u.BOOT_PATH: record(),
                      '/chime/slot0.bin': image(1), '/chime/original.wav': b'sound',
                      '/chime/config.bin': b'existing-config'}
        self.dev = UpdateDevice(self.files)
        self.installer = p.Installer(self.dev, self.root, report=lambda _: None)

    def run_update(self):
        u.update(self.installer, self.package, self.allowed)

    def test_stages_inactive_slot_then_arms_trial(self):
        self.run_update()
        self.assertEqual([x[0] for x in self.dev.writes], ['/chime/slot1.bin'])
        self.assertEqual(self.dev.activations, 1)
        self.assertEqual(u.parse_record(self.dev.files[u.BOOT_PATH]), (0, 1, 0, 2))
        for path in self.files:
            if path != u.BOOT_PATH:
                self.assertEqual(self.dev.files[path], self.files[path])

    def test_failure_during_inactive_write_cannot_activate(self):
        self.dev.write_outcome = 'corrupt'
        with self.assertRaisesRegex(ValueError, 'write could not be verified'):
            self.run_update()
        self.assertEqual(self.dev.activations, 0)
        self.assertEqual(self.dev.files[u.BOOT_PATH], record())

    def test_lost_ack_after_activation_checks_without_repeating_write(self):
        self.dev.activation = 'lost-ack'
        self.run_update()
        self.assertEqual(self.dev.activations, 1)
        self.assertEqual(u.parse_record(self.dev.files[u.BOOT_PATH])[1], 1)

    def test_uncommitted_activation_stops_with_old_record(self):
        self.dev.activation = 'unchanged'
        with self.assertRaisesRegex(ValueError, 'Activation not verified'):
            self.run_update()
        self.assertEqual(self.dev.activations, 1)
        self.assertEqual(self.dev.files[u.BOOT_PATH], record())

    def test_resume_verified_inactive_image_skips_upload(self):
        self.dev.files['/chime/slot1.bin'] = image(2)
        self.run_update()
        self.assertEqual(self.dev.writes, [])
        self.assertEqual(self.dev.activations, 1)

    def test_already_pending_exact_image_is_read_only(self):
        self.dev.files[u.BOOT_PATH] = record(pending=1)
        self.dev.files['/chime/slot1.bin'] = image(2)
        self.run_update()
        self.assertEqual(self.dev.writes, [])
        self.assertEqual(self.dev.activations, 0)

    def test_different_pending_image_or_attempted_trial_is_not_overwritten(self):
        for tried in [0, 1]:
            with self.subTest(tried=tried):
                self.dev.files[u.BOOT_PATH] = record(pending=1, tried=tried)
                self.dev.files['/chime/slot1.bin'] = image(3)
                with self.assertRaises(ValueError):
                    self.run_update()
                self.assertEqual(self.dev.writes, [])
                self.assertEqual(self.dev.activations, 0)

    def test_already_active_is_read_only(self):
        self.dev.files['/chime/slot0.bin'] = image(2)
        self.run_update()
        self.assertEqual(self.dev.writes, [])
        self.assertEqual(self.dev.activations, 0)

    def test_unknown_loader_rejected(self):
        self.dev.files['/sys/mcuimg.bin'] = b'other-loader'
        with self.assertRaisesRegex(ValueError, 'Unknown loader'):
            self.run_update()
        self.assertEqual(self.dev.writes, [])

    def test_corrupt_active_or_record_rejected(self):
        for target in ['/chime/slot0.bin', u.BOOT_PATH]:
            with self.subTest(target=target):
                saved = self.dev.files[target]
                self.dev.files[target] = b'!' + saved[1:]
                with self.assertRaises(ValueError):
                    self.run_update()
                self.assertEqual(self.dev.writes, [])
                self.dev.files[target] = saved

    def test_record_changed_during_upload_prevents_activation(self):
        original_write = self.dev.write
        def write(*args):
            original_write(*args)
            self.dev.files[u.BOOT_PATH] = record(generation=9)
        self.dev.write = write
        with self.assertRaisesRegex(ValueError, 'selection changed'):
            self.run_update()
        self.assertEqual(self.dev.activations, 0)

    def test_image_and_vectors_validated_before_any_io(self):
        self.package.write_bytes(image(2)[:-1])
        with self.assertRaises(ValueError):
            self.run_update()
        self.assertEqual(self.dev.connections, 0)
        bad = bytearray(image(2))
        struct.pack_into('<I', bad, 64, 0x20000000)
        bad[32:64] = hashlib.sha256(bad[64:]).digest()
        with self.assertRaisesRegex(ValueError, 'vectors'):
            u.validate_image(bad)

    def test_mirrored_record_adapter_opens_existing_without_erasing(self):
        device = p.SerialDevice('unused')
        device.connection = Mock()
        device.connection._get_file_info.return_value = type('Info', (), {'exists': True, 'size': 16})()
        device.connection._get_last_status.return_value.is_ok = True
        data = record(pending=1)
        device.update_boot_record(data)
        device.connection._open_file.assert_called_once_with(u.BOOT_PATH, 0x1000)
        device.connection._send_packet.assert_called_once_with(b'\x24' + struct.pack('>I', 0) + data)
        device.connection.erase_file.assert_not_called()
        device.connection.write_file.assert_not_called()
        device.connection._close_file.assert_called_once()


if __name__ == '__main__':
    unittest.main(verbosity=2)
