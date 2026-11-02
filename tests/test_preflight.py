"""Verify that public installation tools reject unsafe or device-specific inputs."""
import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import provision as p
from test_provision import Device


class Preflight(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        manifest = {}
        for name, _, _, _ in p.STAGES:
            data = b'build artifact'
            (self.root / name).write_bytes(data)
            manifest[name] = {'size': len(data), 'sha256': hashlib.sha256(data).hexdigest()}
        (self.root / 'manifest.json').write_text(json.dumps(manifest))
        self.out = patch.object(p, 'OUT', self.root)
        self.out.start()
        self.addCleanup(self.out.stop)

    def test_plan_needs_no_private_device_files(self):
        self.assertIsNone(p.preflight())

    def test_artifact_tampering_stops_before_serial_access(self):
        (self.root / p.STAGES[0][0]).write_bytes(b'tampered')
        with self.assertRaisesRegex(ValueError, 'Artifact mismatch'):
            p.preflight()

    def test_configured_device_is_not_reprovisioned(self):
        device = Device({'/chime/config.bin': b'configured'})
        installer = p.Installer(device, self.root, report=lambda _: None)
        with self.assertRaisesRegex(ValueError, 'Already configured'):
            installer.install(self.root)
        self.assertEqual(device.writes, [])
        self.assertEqual(device.reads, [])

    def test_custom_sound_device_is_not_reprovisioned(self):
        device = Device({'/chime/sound.bin': b'custom sound selection'})
        installer = p.Installer(device, self.root, report=lambda _: None)
        with self.assertRaisesRegex(ValueError, 'Already configured'):
            installer.install(self.root)
        self.assertEqual(device.writes, [])
        self.assertEqual(device.reads, [])

    def test_cli_has_no_factory_extraction_options(self):
        from contextlib import redirect_stdout
        import io
        with redirect_stdout(io.StringIO()) as out, self.assertRaises(SystemExit):
            p.main(['--help'])
        text = out.getvalue()
        self.assertNotIn('--capture-loader', text)
        self.assertNotIn('--backup-a', text)
        self.assertIn('replaces the original startup firmware', ' '.join(text.split()))


if __name__ == '__main__':
    unittest.main(verbosity=2)
