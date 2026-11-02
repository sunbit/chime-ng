#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 Chime NG contributors
"""Install/resume the first UART provisioning; default is an offline plan.

Existing matching Chime NG data files are read, never rewritten. The bootloader is replaced
last. No serial port is opened without --write. A reset is requested after each
communication failure; readback continues at the last checksum-validated block.
"""
import argparse
import hashlib
import json
import logging
import os
import re
from pathlib import Path
import struct
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / '.pio/build/chime-en'
VERSION = re.search(r'#define FW_VERSION "([0-9.]+)"', (ROOT / 'include/core.h').read_text())[1]
STAGES = [
    (f'chime-ng-{VERSION}.chime', '/chime/slot0.bin', 131136, False),
    ('chime.wav', '/chime/original.wav', 228396, False),
    ('boot-initial.bin', '/chime/boot.bin', 16, True),
    ('chime-bootloader.bin', '/sys/mcuimg.bin', 16384, True),
]


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


class LinkError(Exception):
    """The UART session must be discarded before another operation."""


class SerialDevice:
    """Thin adapter over the installed cc3200tool; no package modifications."""

    def __init__(self, port):
        # Lazy imports keep planning usable without opening or importing a UART.
        import serial
        from cc3200tool import cc
        self.serial, self.cc, self.name = serial, cc, port
        self.connection = self.port = None
        self.errors = (cc.CC3200Error, serial.SerialException)

    def close(self):
        if self.port is not None:
            self.port.close()
        self.connection = self.port = None

    def connect(self):
        # An unavailable USB port cannot reach the interactive reset prompt.
        # Stop instead of spinning endlessly before the user can intervene.
        try:
            self.port = self.serial.Serial(
                self.name, baudrate=self.cc.CC3200_BAUD,
                parity=self.serial.PARITY_NONE, stopbits=self.serial.STOPBITS_ONE,
                xonxoff=False, rtscts=False, dsrdtr=False)
        except self.serial.SerialException as exc:
            raise OSError(f'Cannot open {self.name}: {exc}') from exc
        try:
            self.connection = self.cc.CC3200Connection(
                self.port, self.cc.Pincfg(False, 'prompt'), self.cc.Pincfg(False, 'none'))
            self.connection.connect()
            if not self.connection.vinfo.is_cc3200:
                raise ValueError('Target is not a CC3200; stopped before writes.')
            self.connection.switch_to_nwp_bootloader()
            info = self.connection._get_storage_info(self.cc.STORAGE_ID_SFLASH)
            if info.block_size * info.block_count != 8388608:
                raise ValueError('Unexpected flash capacity; stopped before writes.')
        except self.errors as exc:
            raise LinkError(str(exc)) from exc

    def info(self, target):
        try:
            info = self.connection._get_file_info(target)
            return info.size if info.exists else None
        except self.errors as exc:
            raise LinkError(str(exc)) from exc

    def open_read(self, target):
        try:
            self.connection._open_file_for_read(target)
        except self.errors as exc:
            raise LinkError(str(exc)) from exc

    def read(self, offset, size):
        try:
            self.connection._send_packet(
                self.cc.OPCODE_READ_FILE_CHUNK + struct.pack('>II', offset, size))
            data = self.connection._read_packet()  # checksum verified here
            if len(data) != size:
                raise LinkError(f'Unexpected read length at 0x{offset:x}')
            return data
        except self.errors as exc:
            raise LinkError(str(exc)) from exc

    def close_read(self):
        try:
            self.connection._close_file()
        except self.errors as exc:
            raise LinkError(str(exc)) from exc

    def write(self, source, target, capacity, mirror):
        try:
            with source.open('rb') as stream:
                self.connection.write_file(stream, target, size=capacity, commit_flag=mirror)
        except self.errors as exc:
            raise LinkError(str(exc)) from exc

    def update_boot_record(self, data):
        """Open the existing mirrored record for write, without deleting it.

        TI fs.h encodes FS_MODE_OPEN_WRITE as access=1 at bit 12. The
        existing file keeps its COMMIT/mirror allocation; close commits it.
        This narrow operation is only used after the inactive image verifies.
        """
        if len(data) != 16:
            raise ValueError('Boot record must be exactly 16 bytes')
        try:
            info = self.connection._get_file_info('/chime/boot.bin')
            if not info.exists or info.size != 16:
                raise ValueError('Existing boot record is missing or has changed size')
            self.connection._open_file('/chime/boot.bin', self.cc.SLFS_MODE_OPEN_WRITE << 12)
            self.connection._send_packet(self.cc.OPCODE_FILE_CHUNK + struct.pack('>I', 0) + data)
            if not self.connection._get_last_status().is_ok:
                raise LinkError('Boot record write returned an error')
            self.connection._close_file()
        except self.errors as exc:
            raise LinkError(str(exc)) from exc


class Installer:
    def __init__(self, device, directory, chunk=1024, report=print):
        self.device, self.directory, self.chunk = device, Path(directory), chunk
        self.report = report
        self.connected = False
        self.read_number = 0

    def event(self, message):
        self.report(message)
        with (self.directory / 'events.log').open('a') as stream:
            stream.write(message + '\n')
            stream.flush()
            os.fsync(stream.fileno())

    def disconnect(self):
        try:
            self.device.close()
        finally:
            self.connected = False

    def recover(self, exc):
        self.disconnect()
        self.event(f'\nUART: {exc}\nKeep SOP2 high. Reset when prompted; Enter retries, Ctrl-C stops.')

    def connect(self):
        while not self.connected:
            try:
                self.device.connect()
                self.connected = True
            except LinkError as exc:
                self.recover(exc)

    def read_file(self, target, maximum):
        """Retain checksum-validated blocks only during this invocation.

        A restarted installer always reads the device again. Saved readbacks are
        evidence, never authority to skip a future device check. Keep the same
        board in SOP2 bootloader mode throughout retries (no normal boot).
        """
        self.read_number += 1
        path = self.directory / f'{self.read_number:02d}-{target.rsplit("/", 1)[-1]}.readback'
        data = bytearray()
        expected_size = None
        with path.open('wb') as stream:
            while True:
                self.connect()
                try:
                    size = self.device.info(target)
                    if size != self.device.info(target):
                        raise LinkError('File metadata reads disagree; discarded')
                    if size is not None and not 0 <= size <= maximum:
                        raise ValueError(f'{target}: unexpected size {size}; stopped before rewriting it.')
                    if expected_size is not None and size != expected_size:
                        raise ValueError(f'{target}: file size changed during readback; refusing to mix reads.')
                    if size is None:
                        self.event(f'Absent: {target}')
                        return None
                    expected_size = size
                    self.device.open_read(target)
                    self.event(f'Reading {target}: {len(data)}/{size} bytes; chunk={self.chunk}')
                    while len(data) < size:
                        block = self.device.read(len(data), min(self.chunk, size - len(data)))
                        if len(block) != min(self.chunk, size - len(data)):
                            raise LinkError('Incomplete read block; discarded')
                        stream.write(block)
                        stream.flush()
                        data.extend(block)
                        if len(data) % 16384 == 0 or len(data) == size:
                            self.event(f'  Read {len(data)}/{size} bytes')
                    self.device.close_read()
                    os.fsync(stream.fileno())
                    self.event(f'Readback SHA256: {hashlib.sha256(data).hexdigest()}')
                    return bytes(data)
                except LinkError as exc:
                    # Never send another command into a desynchronized session.
                    # Reopen the file after a physical reset, with an explicit offset.
                    self.recover(exc)

    def check(self, target, candidates, maximum):
        """Return matching candidate index, or None after a confirmed mismatch.

        A checksum-valid but incorrect payload is possible with an 8-bit checksum.
        Do not erase a file based on one mismatch: independently reread it after
        reset. Nonidentical reads halt installation instead of authorizing writes.
        """
        first = self.read_file(target, maximum)
        if first in candidates:
            return candidates.index(first)
        self.event(f'{target}: does not match; confirming with a fresh read before any write.')
        self.disconnect()
        second = self.read_file(target, maximum)
        if second in candidates:
            return candidates.index(second)
        if first != second:
            raise ValueError(f'{target}: independent reads differ. No write authorized by this check.')
        return None

    def ensure(self, source, target, capacity, mirror, read_only=False):
        expected = source.read_bytes()
        if self.check(target, [expected], capacity) == 0:
            self.event(f'Verified, no write needed: {target}')
            return
        if read_only:
            raise ValueError(f'{target}: differs on an already-installed Chime NG. No writes; use OTA.')
        self.write_and_verify(source, target, capacity, mirror)

    def write_and_verify(self, source, target, capacity, mirror):
        expected = source.read_bytes()
        self.event(f'Writing {target}; readback verification follows before the next stage.')
        self.connect()
        try:
            self.device.write(source, target, capacity, mirror)
        except LinkError as exc:
            # An ACK may have been lost after a successful flash operation.
            # Inspect the target first; never automatically repeat erase/write.
            self.recover(exc)
            self.event('Write outcome uncertain. Reading the target before considering any further operation.')
        if self.check(target, [expected], capacity) != 0:
            raise ValueError(f'{target}: write could not be verified. Later stages NOT executed. '
                             'Keep SOP2 high; inspect this run before retrying.')
        self.event(f'Written and verified: {target}')

    def install(self, artifacts, resume=False):
        # Inspect only this project's namespace. Do not read or save the factory
        # application/loader before replacing it during a first installation.
        for target in ['/chime/config.bin', '/chime/sound.bin']:
            while True:
                self.connect()
                try:
                    if self.device.info(target) is not None:
                        raise ValueError('Already configured Chime NG: use web OTA or the UART updater, not initial installation.')
                    break
                except LinkError as exc:
                    self.recover(exc)
        missing = []
        # Check every Chime NG prerequisite before any writes. A different
        # existing application or boot record belongs to the recovery path.
        for filename, target, capacity, mirror in STAGES[:-1]:
            source = artifacts / filename
            mode = self.check(target, [source.read_bytes(), None], capacity)
            if mode is None and not resume:
                raise ValueError(f'{target}: differs on an already-installed or conflicting Chime NG. No writes; use OTA or recovery.')
            if mode != 0:
                missing.append((source, target, capacity, mirror))
            else:
                self.event(f'Verified, no write needed: {target}')
        for source, target, capacity, mirror in missing:
            mode = self.check(target, [source.read_bytes(), None], capacity)
            if mode is None and not resume:
                raise ValueError(f'{target}: changed during installation; no replacement authorized.')
            if mode != 0:
                self.write_and_verify(source, target, capacity, mirror)
        # Replace the ROM-loadable image last, then verify only our new bytes.
        # Repeating initial installation repeats this loader write; it does not
        # require or extract any original vendor image for a comparison.
        self.write_and_verify(artifacts / STAGES[-1][0], STAGES[-1][1], 16384, True)
        self.event('INSTALLATION VERIFIED: every target matches the local artifact byte-for-byte.')


def preflight():
    manifest = json.loads((OUT / 'manifest.json').read_text())
    for filename, _, capacity, _ in STAGES:
        info = manifest[filename]
        path = OUT / filename
        if (not 0 < path.stat().st_size <= capacity or
                path.stat().st_size != info['size'] or digest(path) != info['sha256']):
            raise ValueError(f'Artifact mismatch: {filename}. Stopped before serial access.')


def main(argv=None):
    global OUT
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('-p', '--port', help='Your USB UART port; required for serial operations')
    parser.add_argument('--artifacts', type=Path, default=OUT)
    parser.add_argument('--write', action='store_true', help='Initial installation: replaces the original startup firmware. Manual SOP2/reset.')
    parser.add_argument('--resume-initial', action='store_true', help='Explicitly allow replacing incomplete Chime NG data from a failed first installation.')
    parser.add_argument('--read-chunk', type=int, default=1024, help='Readback block bytes, 1..4096 (default: 1024).')
    args = parser.parse_args(argv)
    if not 1 <= args.read_chunk <= 4096:
        parser.error('--read-chunk must be 1..4096')
    OUT = args.artifacts
    if args.write and not args.port:
        parser.error('--port is required for serial operations')
    preflight()
    print('Build artifacts verified.', flush=True)
    print('WARNING: initial flashing replaces the original firmware. No factory restore procedure is provided.', flush=True)
    print('Planned order (matching Chime NG data is skipped; the loader is written last):', flush=True)
    for filename, target, _, _ in STAGES:
        print(f'  {filename} -> {target}', flush=True)
    if not args.write:
        print('\nPLAN ONLY: serial port was not opened. Add --write to install/resume.')
        return 0
    directory = Path(tempfile.mkdtemp(prefix='chime-install-', dir=OUT))
    print(f'Run evidence and readbacks: {directory}', flush=True)
    device = SerialDevice(args.port)
    handler = logging.FileHandler(directory / 'uart.log')
    handler.setFormatter(logging.Formatter('%(asctime)s -- %(message)s'))
    logging.getLogger().addHandler(handler)
    installer = Installer(device, directory, args.read_chunk, report=lambda s: print(s, flush=True))
    try:
        installer.install(OUT, resume=args.resume_initial)
    finally:
        installer.disconnect()
        logging.getLogger().removeHandler(handler)
        handler.close()
    print('\nRemove SOP2 pull-up and reset to run Chime NG.')
    print('Setup Wi-Fi: ChimeSetup-<MAC suffix>; browser: http://192.168.4.1')
    print(f'Setup password: read {OUT / "setup-password.txt"}')
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (OSError, ValueError, KeyboardInterrupt, EOFError) as exc:
        print(f'\nInstallation stopped: {exc or "interrupted"}. Run evidence is preserved. '
              'Keep SOP2 high until the installation is verified.', file=sys.stderr)
        sys.exit(1)
