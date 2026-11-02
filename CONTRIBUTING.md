# Contributing

Keep source, comments, documentation and the default interface in English.
Use `pio run` for the embedded build and `python tests/run.py` for offline
verification. Keep TI source headers intact and update provenance if the
vendor subset changes. Changes to pin mappings, flash layout, boot selection
or audio startup need evidence and relevant fault tests.

Report board/chip revision, firmware version, a minimal reproduction and a
sanitized diagnostic. Do not attach full dumps, configuration blobs,
passwords, tokens, certificates with private keys, or raw network logs.
Distinguish hardware observations from conclusions drawn from simulated tests.

Protocol topic IDs and the `original` sound token are compatibility interfaces.
Document changes and increment the Homie description version when its
declaration changes. Never remove UART recovery paths merely to simplify OTA.
