#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 Chime NG contributors
"""Stage a Chime NG application over UART when the web cannot be reached.

Default: offline validation only. With --write, preserves the active image,
loader, configuration and WAV, verifies the inactive image, then arms one boot
trial through the existing mirrored boot record. Never uses initial provisioning.
"""
import argparse
import hashlib
import logging
from pathlib import Path
import struct
import sys
import tempfile
import zlib

from provision import OUT, STAGES, Installer, LinkError, SerialDevice, preflight

MAX_PACKAGE = 131136
BOOT_PATH = '/chime/boot.bin'


def validate_image(data):
    if data is None or len(data) < 64 + 1024:
        raise ValueError('Missing/short application image')
    magic, board, fmt, address, size, build, flags, digest = struct.unpack('<8s6I32s', data[:64])
    if (magic != b'CHIMNG01' or board != 0x43483332 or fmt != 1 or
            address != 0x20010000 or not 1024 <= size <= 131072 or flags or len(data) != size + 64):
        raise ValueError('Invalid image header, board, address or length')
    if hashlib.sha256(data[64:]).digest() != digest:
        raise ValueError('Application SHA256 mismatch')
    sp, pc = struct.unpack('<II', data[64:72])
    if not (address + size < sp <= 0x20040000 and sp % 8 == 0 and pc & 1 and
            address <= (pc & ~1) < address + size):
        raise ValueError('Invalid application vectors')
    return build


def parse_record(data):
    if data is None or len(data) != 16:
        raise ValueError('Missing boot record')
    magic, active, pending, tried, reserved, generation, crc = struct.unpack('<I4BII', data)
    if (magic != 0x43484231 or active not in (0, 1) or pending not in (0, 1, 255) or
            tried not in (0, 1) or reserved or (pending != 255 and pending == active) or
            (pending == 255 and tried) or zlib.crc32(data[:12]) != crc):
        raise ValueError('Invalid boot record; no writes')
    return active, pending, tried, generation


def trial_record(active, generation):
    data = struct.pack('<I4BI', 0x43484231, active, 1 - active, 0, 0, (generation + 1) & 0xffffffff)
    return data + struct.pack('<I', zlib.crc32(data))


def update(installer, package, allowed_loader_hashes):
    expected = package.read_bytes()
    validate_image(expected)
    initial = installer.read_file(BOOT_PATH, 16)
    active, pending, tried, generation = parse_record(initial)
    loader = installer.read_file('/sys/mcuimg.bin', 16384)
    if loader is None or hashlib.sha256(loader).hexdigest() not in allowed_loader_hashes:
        raise ValueError('Unknown loader; no writes. This updater requires an installed Chime NG loader.')
    active_path = f'/chime/slot{active}.bin'
    previous = installer.read_file(active_path, MAX_PACKAGE)
    validate_image(previous)
    if pending != 255:
        if tried:
            raise ValueError('An unconfirmed trial has already run. Boot normally to roll back before staging another update.')
        staged = installer.read_file(f'/chime/slot{pending}.bin', MAX_PACKAGE)
        if staged != expected:
            raise ValueError('Another image is pending; no writes.')
        installer.event('UPDATE STAGED AND VERIFIED: this exact image was already pending. No writes needed.')
        return
    if previous == expected:
        installer.event('ALREADY ACTIVE: this exact application is selected and verified. No writes needed.')
        return
    target = f'/chime/slot{1-active}.bin'
    installer.event(f'Preserving {active_path}. Staging {target}.')
    installer.ensure(package, target, MAX_PACKAGE, False)
    if installer.check(BOOT_PATH, [initial], 16) != 0:
        raise ValueError('Boot selection changed during upload. Stopped before activation.')
    planned = trial_record(active, generation)
    (installer.directory / 'planned-boot.bin').write_bytes(planned)
    installer.event('Inactive image verified. Updating the existing mirrored boot record without erasing it.')
    installer.connect()
    try:
        installer.device.update_boot_record(planned)
    except LinkError as exc:
        installer.recover(exc)
        installer.event('Activation result uncertain: checking the record; not repeating the write.')
    if installer.check(BOOT_PATH, [planned], 16) != 0:
        raise ValueError('Activation not verified. Preserve this log and keep SOP2 high; no loader or active-image writes were performed.')
    installer.event('UPDATE STAGED AND VERIFIED: remove SOP2 pull-up and reset for one trial boot.')


def main():
    global OUT
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('-p', '--port')
    parser.add_argument('--artifacts', type=Path, default=OUT)
    parser.add_argument('--installed-loader', type=Path, help='Trusted Chime NG loader binary from its original build')
    parser.add_argument('--write', action='store_true')
    parser.add_argument('--read-chunk', type=int, default=1024)
    args = parser.parse_args()
    if not 1 <= args.read_chunk <= 4096:
        parser.error('--read-chunk must be 1..4096')
    OUT = args.artifacts
    import provision
    provision.OUT = OUT
    preflight()
    if args.write and not args.port:
        parser.error('--port is required with --write')
    package = OUT / STAGES[0][0]
    build = validate_image(package.read_bytes())
    allowed = {hashlib.sha256((OUT / 'chime-bootloader.bin').read_bytes()).hexdigest()}
    if args.installed_loader:
        allowed.add(hashlib.sha256(args.installed_loader.read_bytes()).hexdigest())
    print(f'Package verified: {package.name}; build={build}; bytes={package.stat().st_size}', flush=True)
    print('Plan: verify loader and active image; stage inactive image; verify; update mirrored boot record.', flush=True)
    if not args.write:
        print('PLAN ONLY: serial port was not opened. Add --write to stage this update.')
        return
    directory = Path(tempfile.mkdtemp(prefix='chime-update-', dir=OUT))
    print(f'Run evidence: {directory}', flush=True)
    device = SerialDevice(args.port)
    handler = logging.FileHandler(directory / 'uart.log')
    handler.setFormatter(logging.Formatter('%(asctime)s -- %(message)s'))
    logging.getLogger().addHandler(handler)
    installer = Installer(device, directory, args.read_chunk, report=lambda s: print(s, flush=True))
    try:
        update(installer, package, allowed)
    finally:
        installer.disconnect()
        logging.getLogger().removeHandler(handler)
        handler.close()
    print('After normal boot: check system/firmware, system/ip, system/web and chime/diagnostic on MQTT.')


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, KeyboardInterrupt, EOFError) as exc:
        print(f'\nUpdate stopped: {exc or "interrupted"}. Run evidence is preserved.', file=sys.stderr)
        sys.exit(1)
