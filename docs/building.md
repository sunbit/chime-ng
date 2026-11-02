# Build and development

## Supported build path

Use Python 3.11 or later, Git and PlatformIO Core 6.1.19.
macOS Apple Silicon was tested locally. Linux is covered by the included CI
workflow; other hosts, including Windows, have not been verified here.

```sh
python3 -m venv .venv
.venv/bin/python -m pip install -r requirements.txt
.venv/bin/pio run -e chime-en
```

The first run downloads `platformio/toolchain-gccarmnoneeabi@1.120301.0`
(GCC 12.3.1) and SCons. TI driver sources are included in `vendor/ti/`; no
separate SDK installation is needed. UART programming dependencies are
installed separately through `tools/requirements.txt`.
`vendor/ti/PROVENANCE.json` records the upstream commit and SHA-256
of every vendored file.

PlatformIO uses the local CC3200 platform in `platforms/cc3200/` to compile
the application and loader, then generate the `.chime` package and initial
boot record.

## Outputs

All paths below are relative to `.pio/build/chime-en/`:

| File | Use |
| --- | --- |
| `chime-bootloader.bin` | ROM-loadable relocating loader; initial UART installation |
| `chime-ng-0.2.1.chime` | Application package; UART slot or web OTA |
| `boot-initial.bin` | Initial mirrored boot selection record |
| `chime.wav` | Synthesized sound for initial UART installation |
| `manifest.json` | Artifact sizes and SHA-256 digests |
| `setup-password.txt` | Private initial AP and web password, created with mode 0600 |
| `chime-app.elf`, `boot-body.elf`, `*.map` | Symbols and memory layout for debugging |

**A `.chime` package is not a TI `mcuimg.bin`.** Never write it directly to
`/sys/mcuimg.bin`; that path must contain `chime-bootloader.bin`.

The application is limited to 128 KiB and linked at `0x20010000`. The loader
runs below it at `0x20000000`, after a ROM entry at `0x20004000` relocates it.
Linker assertions enforce the image and minimum stack bounds.

GCC 12 can warn about fixed-address TI ROM table accesses and RWX load
segments. The RWX segments reflect SRAM execution on this MCU.

## Language at compile time

`custom_language = en` in `platformio.ini` selects `web/en.html` and
`locales/en.json`. To add a translation, create matching HTML and JSON files,
copy the English environment under a new name, and set `custom_language`
to that filename stem.
The JSON keys must match the English file exactly. Invalid language names or
missing files stop the build. Only the chosen HTML and response strings are
compiled into the application; the version badge comes from `FW_VERSION`.

MQTT topic IDs, payloads and diagnostic tokens remain stable and English.
The display name is entered during setup; it is not compiled into the image.

## Reproducibility and passwords

Compiler, PlatformIO Core, UART dependency revision and vendored source are
pinned. Builds use the checked-in WAV. Python transitive dependencies follow
the pinned tools' constraints and are not locked individually.

The first build creates a random setup password. Rebuilds in the same
environment keep it, and firmware contents therefore intentionally differ
between independent first builds. Back up your build's password file privately.
`pio run -t clean` removes build outputs, so save that password before cleaning
if you still need to provision devices with the old image. Never publish a
personal build directory. Firmware binaries embed the setup password.

After changing the custom platform, reinstall its local copy if necessary:

```sh
.venv/bin/pio pkg uninstall --global --platform chime-cc3200
.venv/bin/pio run
```

## Offline verification

Install Clang (`xcode-select --install` on macOS; your distribution's Clang
package on Linux). Then run:

```sh
.venv/bin/python -m pip install -r tools/requirements.txt
.venv/bin/python tests/run.py
```

The runner writes test fixtures to `build/` and runs without connected
hardware or a broker. C integration tests use AddressSanitizer and
UndefinedBehaviorSanitizer. See [test coverage and hardware checks](validation.md).

The UI can be inspected separately with:

```sh
.venv/bin/python tests/preview.py
```

Open `http://127.0.0.1:5059` and enter any test password. This preview uses
simulated device responses. Keep it local and stop it with Ctrl+C.

For routine firmware updates, rebuild with PlatformIO and upload the generated
`.chime` file through the device's web interface. The Python UART tools are
only for first installation (`tools/provision.py`) or recovery when the web
is unavailable (`tools/update_uart.py`). Follow the [installation and update
guide](flashing.md) for each procedure.
