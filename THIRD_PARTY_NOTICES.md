# Third-party notices

## TI CC3200 driverlib and SimpleLink host driver

`vendor/ti/` contains an unmodified subset of
[Energia cc3200-core](https://github.com/energia/cc3200-core), commit
`c5507401fe46454d38c3ce3af0a140df8fc088a3`: TI headers plus the 24 C source
files used by this build. Each file retains its TI copyright and BSD-style
redistribution conditions. `vendor/ti/PROVENANCE.json` records all source
hashes. See [the retained TI license](licenses/TI-BSD.txt).

The subset contains 78 files with TI copyright notices from 2014 and 2015.

This build does not link the Energia Arduino core or WiFi C++ wrapper.
The LGPL `wl_definitions.h` wrapper header is not vendored or used.
Redistributed firmware binaries must be accompanied by these TI notices.
TI and contributor names must not be used to imply endorsement.

## Host tools and build dependencies

- `cc3200tool`, pinned to ALLTERCO commit
  `6a066d836e8194dff766f7c76035b3c944531d86`, is a separate GPL-2.0-or-later
  Python host tool. The original project source is at
  [ALLTERCO/cc3200tool](https://github.com/ALLTERCO/cc3200tool).
  Chime NG's importing UART scripts are supplied under compatible GPL terms;
  [GPL version 2](licenses/GPL-2.0.txt) is included. They are not linked into
  the embedded firmware.
- PlatformIO Core is Apache-2.0 software, installed as a development tool.
- The ARM GNU compiler, assembler, linker and runtime libraries retain their
  own licenses and runtime exceptions; consult their upstream license notices
  for the exact package version. They are downloaded by PlatformIO, not
  copied into this repo.
- Python, pyserial, setuptools, SCons and Clang are external development
  dependencies and retain their own licenses.

The upstream UART package includes TI RAM helper binaries named `rbtl*.dll`.
They are downloaded with `tools/requirements.txt` and are not bundled here.
Check their TI redistribution terms before including them in a tool bundle;
`cc3200tool`'s GPL license does not establish those permissions. See
[licensing guidance](docs/licensing.md).

## Sound, photographs and original firmware

`assets/chime.wav` was synthesized from sine waves for this project and is
MIT licensed. It contains no Ring recordings or samples. Ring firmware and
audio are not distributed.

The hardware photographs and contact diagram are MIT-licensed project material.
Ring, Amazon, Texas Instruments and other product names remain their owners'
trademarks.
