#!/usr/bin/env python3
"""UART installer regressions with a simulated filesystem; never open a port."""
from contextlib import redirect_stdout
import io
from pathlib import Path
import struct
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import provision as p


class Device:
    def __init__(self, files):
        self.files = files.copy()
        self.current = None
        self.connections = 0
        self.reads = []
        self.writes = []
        self.fail_reads = {}
        self.fail_connects = 0
        self.fail_close = 0
        self.write_outcome = 'ok'
        self.alter = None
        self.info_hook = None

    def connect(self):
        self.connections += 1
        if self.fail_connects:
            self.fail_connects -= 1
            raise p.LinkError('Did not get ACK on break condition')

    def close(self):
        self.current = None

    def info(self, target):
        if self.info_hook:
            return self.info_hook(target)
        return len(self.files[target]) if target in self.files else None

    def open_read(self, target):
        self.current = target

    def close_read(self):
        if self.fail_close:
            self.fail_close -= 1
            raise p.LinkError('No ACK closing read')
        self.current = None

    def read(self, offset, size):
        key = self.current, offset
        self.reads.append((self.current, offset, size))
        if self.fail_reads.get(key, 0):
            self.fail_reads[key] -= 1
            raise p.LinkError('rx csum failed')
        data = self.files[self.current][offset:offset + size]
        return self.alter(self.current, offset, data) if self.alter else data

    def write(self, source, target, capacity, mirror):
        self.writes.append((target, capacity, mirror))
        if self.write_outcome == 'missing':
            self.files.pop(target, None)
            raise p.LinkError('ACK lost after erase')
        data = source.read_bytes()
        self.files[target] = b'!' + data[1:] if self.write_outcome == 'corrupt' else data
        if self.write_outcome == 'lost_ack':
            raise p.LinkError('ACK lost after close')


class ProvisionTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.original = self.root / 'ring.bin'
        self.original.write_bytes(b'original-ring-loader')
        self.expected = {}
        for i, (name, target, _, _) in enumerate(p.STAGES):
            data = bytes([i + 1]) * (3100 if 'wav' in name else 11)
            (self.root / name).write_bytes(data)
            self.expected[target] = data
        self.boot = p.STAGES[-1][1]
        self.wav = '/chime/original.wav'
        self.slot = '/chime/slot0.bin'

    def installer(self, files):
        device = Device(files)
        installer = p.Installer(device, self.root, chunk=1024, report=lambda _: None)
        return installer, device

    def test_reported_failure_resumes_wav_without_rewriting_slot_or_wav(self):
        files = {self.boot: self.original.read_bytes(),
                 self.slot: self.expected[self.slot], self.wav: self.expected[self.wav]}
        installer, dev = self.installer(files)
        dev.fail_reads[self.wav, 1024] = 2
        installer.install(self.root)
        self.assertEqual([w[0] for w in dev.writes], ['/chime/boot.bin', self.boot])
        self.assertEqual(sum(t == self.wav and o == 0 for t, o, _ in dev.reads), 1)
        self.assertEqual(sum(t == self.wav and o == 1024 for t, o, _ in dev.reads), 3)
        self.assertEqual(dev.files, self.expected)

    def test_empty_own_files_written_in_order_boot_last(self):
        installer, dev = self.installer({self.boot: self.original.read_bytes()})
        installer.install(self.root)
        self.assertEqual(dev.writes, [(t, c, m) for _, t, c, m in p.STAGES])

    def test_connection_failures_retry_before_any_mutation(self):
        installer, dev = self.installer(self.expected)
        dev.fail_connects = 2
        installer.install(self.root)
        self.assertEqual(dev.connections, 3)
        self.assertEqual([w[0] for w in dev.writes], [self.boot])

    def test_repeated_initial_install_only_rewrites_own_loader(self):
        installer, dev = self.installer(self.expected)
        installer.install(self.root)
        self.assertEqual([w[0] for w in dev.writes], [self.boot])

    def test_existing_new_bootloader_never_resets_ota_state(self):
        files = {**self.expected, '/chime/boot.bin': b'ota-record'}
        installer, dev = self.installer(files)
        with self.assertRaisesRegex(ValueError, 'already-installed'):
            installer.install(self.root)
        self.assertEqual(dev.writes, [])

    def test_factory_loader_is_never_read_before_replacement(self):
        installer, dev = self.installer({self.boot: self.original.read_bytes()})
        def alter(target, offset, data):
            if target == self.boot:
                self.assertEqual(data, self.expected[self.boot][offset:offset+len(data)])
                self.assertTrue(any(w[0] == self.boot for w in dev.writes))
            return data
        dev.alter = alter
        installer.install(self.root)
        self.assertEqual(dev.files, self.expected)

    def test_checksum_valid_corruption_rechecked_before_write(self):
        installer, dev = self.installer(self.expected)
        first = True
        def alter(target, offset, data):
            nonlocal first
            if target == self.wav and offset == 1024 and first:
                first = False
                return b'!' + data[1:]
            return data
        dev.alter = alter
        installer.ensure(self.root / 'chime.wav', self.wav, 228396, False)
        self.assertEqual(dev.writes, [])
        self.assertEqual(dev.connections, 2)

    def test_inconsistent_reads_cannot_authorize_erase(self):
        installer, dev = self.installer(self.expected)
        counter = 10
        def alter(target, offset, data):
            nonlocal counter
            counter += 1
            return bytes([counter]) + data[1:]
        dev.alter = alter
        with self.assertRaisesRegex(ValueError, 'independent reads differ'):
            installer.ensure(self.root / 'chime.wav', self.wav, 228396, False)
        self.assertEqual(dev.writes, [])

    def test_close_read_error_reopens_without_rereading_validated_blocks(self):
        installer, dev = self.installer(self.expected)
        dev.fail_close = 1
        self.assertEqual(installer.read_file(self.wav, 228396), self.expected[self.wav])
        self.assertEqual(len(dev.reads), 4)
        self.assertEqual(dev.connections, 2)

    def test_failed_write_with_matching_readback_is_not_repeated(self):
        installer, dev = self.installer({self.boot: self.original.read_bytes()})
        dev.write_outcome = 'lost_ack'
        installer.install(self.root)
        self.assertEqual([w[0] for w in dev.writes], [s[1] for s in p.STAGES])

    def test_failed_or_corrupt_write_stops_before_later_stages(self):
        for outcome in ['missing', 'corrupt']:
            with self.subTest(outcome=outcome):
                installer, dev = self.installer({self.boot: self.original.read_bytes()})
                dev.write_outcome = outcome
                with self.assertRaisesRegex(ValueError, 'write could not be verified'):
                    installer.install(self.root)
                self.assertEqual([w[0] for w in dev.writes], [self.slot])
                self.assertEqual(dev.files[self.boot], self.original.read_bytes())

    def test_changed_size_after_reset_refuses_to_mix_readback(self):
        installer, dev = self.installer(self.expected)
        calls = 0
        def info(target):
            nonlocal calls
            calls += 1
            return len(dev.files[target]) + (1 if calls > 2 else 0)
        dev.info_hook = info
        dev.fail_reads[self.wav, 1024] = 1
        with self.assertRaisesRegex(ValueError, 'size changed'):
            installer.read_file(self.wav, 228396)
        self.assertEqual(dev.writes, [])

    def test_partial_initial_install_requires_explicit_resume(self):
        installer, dev = self.installer({self.slot: b'partial upload'})
        with self.assertRaisesRegex(ValueError, 'conflicting'):
            installer.install(self.root)
        self.assertEqual(dev.writes, [])
        installer.install(self.root, resume=True)
        self.assertEqual(dev.files, self.expected)
        self.assertEqual(dev.writes[-1][0], self.boot)

    def test_oversized_target_stops_without_write(self):
        installer, dev = self.installer({self.wav: b'x' * 228397})
        with self.assertRaisesRegex(ValueError, 'unexpected size'):
            installer.install(self.root)
        self.assertEqual(dev.writes, [])

    def test_saved_readback_does_not_skip_device_read_on_new_run(self):
        installer, dev = self.installer(self.expected)
        installer.ensure(self.root / 'chime.wav', self.wav, 228396, False)
        dev.files[self.wav] = b'old'
        installer2 = p.Installer(dev, self.root, report=lambda _: None)
        installer2.ensure(self.root / 'chime.wav', self.wav, 228396, False)
        self.assertEqual([w[0] for w in dev.writes], [self.wav])

    def test_dry_run_never_constructs_serial_device(self):
        with patch.object(p, 'preflight', return_value=self.original), \
             patch.object(p, 'SerialDevice', side_effect=AssertionError('Serial access')), \
             redirect_stdout(io.StringIO()) as out:
            self.assertEqual(p.main([]), 0)
        self.assertIn('serial port was not opened', out.getvalue())

    def test_serial_adapter_reads_file_opcode_and_explicit_offset(self):
        device = p.SerialDevice('unused')  # Imports only; does not open a port.
        device.connection = Mock()
        device.connection._read_packet.return_value = b'x' * 1024
        self.assertEqual(device.read(2048, 1024), b'x' * 1024)
        device.connection._send_packet.assert_called_once_with(b'\x2b' + struct.pack('>II', 2048, 1024))
        device.connection._read_packet.side_effect = device.cc.CC3200Error('rx csum failed')
        with self.assertRaises(p.LinkError):
            device.read(2048, 1024)

    def test_unavailable_port_stops_instead_of_retry_loop(self):
        device = p.SerialDevice('unused')
        with patch.object(device.serial, 'Serial', side_effect=device.serial.SerialException('missing')):
            with self.assertRaisesRegex(OSError, 'Cannot open'):
                device.connect()


if __name__ == '__main__':
    unittest.main(verbosity=2)
