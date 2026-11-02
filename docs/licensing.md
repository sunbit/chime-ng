# Licensing and redistribution

| Component | Treatment in this repository | Redistribution action |
| --- | --- | --- |
| Chime NG firmware, web UI, tests, build integration and docs | MIT, see root `LICENSE` | Keep the MIT notice |
| Original synthesized sound | MIT, included as source asset | Keep the project license |
| Hardware photographs and contact diagram | MIT project material | Keep attribution and project license |
| TI driverlib / SimpleLink C and headers | Original BSD-style notices retained | Preserve source headers and include notices with binary releases |
| UART Python scripts importing `cc3200tool` | GPL-2.0-or-later | Provide their source and GPL terms with redistributed tool bundles |
| `cc3200tool` dependency | Downloaded at a fixed source revision | Preserve its GPL/source obligations if redistributing it |
| TI `rbtl*.dll` RAM programming helpers in that dependency | Not copied into the repo or release | Check original TI redistribution terms before bundling |
| ARM GNU/newlib runtime and build tools | Downloaded, not vendored | Review installed runtime licenses/exceptions for a binary distribution |
| Existing TI NWP firmware/service pack on the device | Required on existing hardware; not distributed | Obtain through TI under its applicable terms if replacement is needed |
| Original Ring application, loader and audio | Not distributed | No rights to vendor assets are granted |

The embedded firmware is MIT licensed; the separate Python UART scripts are
GPL-2.0-or-later. TI sources retain their own BSD-style terms. See
[third-party notices](../THIRD_PARTY_NOTICES.md) for upstream revisions and
license texts.

For a **source release**, include this repository, `LICENSE`, the notices,
license texts and vendor provenance; exclude ignored local data and generated
outputs. For a **firmware binary release**, also supply matching source,
the TI notices and the runtime notices relevant to the compiler package you
used. Firmware binaries embed a setup password; do not distribute a personal
build's credentials.

The TI `rbtl*.dll` helpers are downloaded with `tools/requirements.txt` and
are not bundled here. Their independent redistribution permission has not
been established, so do not bundle them based only on `cc3200tool`'s GPL
license.
