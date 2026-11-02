# Validation, limitations and security

## Tested hardware and offline tests

Firmware 0.2.1 has been tested on the board shown in the
[hardware guide](hardware.md). Physical blue LED operation remains unresolved.
The build and offline tests run on macOS Apple Silicon and Ubuntu CI.

The offline suite covers application logic and drivers with simulated
storage, UART, network and peripheral faults. The table below lists its
coverage and the physical checks still needed.

| Area | Offline checks | Physical checks still needed |
| --- | --- | --- |
| Build | Both SRAM images, vector bounds, size/stack assertions and manifests | Additional units and board revisions |
| UART | Readback recovery, independent read consistency, skipped matching files, loader-last installation | Installation and recovery on another unit |
| OTA | Hash/vector/header checks, inactive-slot preservation, trial and rollback under simulated storage faults | Real power-interruption rollback |
| HTTP | Fragmentation, authentication, secret omission, request guards and all routes | Full device web regression |
| MQTT | MQTT 5 frames, QoS 2 handshake, subscriptions, reconnect and command rejection | Additional real brokers and TLS interoperability |
| Audio | Sample output compared with generated WAV, volume, DMA ordering, lost interrupts and bounded failures | Sound audibility and timing |
| Sound upload | Streaming writes, WAV format, full flash readback hash, two slots, mirrored selection and simulated failures | Upload, cold boot and power-interruption behavior on the board |
| LED | GPIO model, timer override, pad enable and latch semantics | Physical light and electrical net diagnosis |
| UI | English login, status, volume and LED controls in local browser fixture | Device browser checks on additional clients |

Offline tests include randomized MQTT/HTTP parser inputs, SHA/CRC reference
vectors, malformed images/WAVs, and ASan/UBSan C integration tests. They cannot
measure hardware waveforms, supply behavior or speaker output.

## Network and credential limits

The web server uses **HTTP**, with Basic authentication and a required custom
header on mutations. Basic authentication does not encrypt credentials.
Use it on a trusted, restricted local network; do not port-forward it to the
Internet. Configuration is stored on external flash without encryption, and
the admin password is stored as a SHA-256 digest without a password-hardening
scheme. Physical flash access can expose network secrets and enable offline
password guessing. Saved passwords are omitted from HTTP status responses.

MQTT TLS requires a CA certificate in DER format and a reachable configured
NTP server IP. It depends on the installed CC3200 network processor/service
pack and its supported TLS versions and ciphers. Compatibility has not been
validated against modern brokers, so do not infer TLS security from a compiled
option alone. Keep the broker local and configure access controls appropriately.

OTA checks SHA-256, board ID, image length, load address and vectors. It does
**not** verify a publisher signature or enforce anti-rollback security.
Anyone with the admin password and network access can upload compatible
code. A valid hash proves integrity, not authenticity. Build from trusted
source and retain physical UART recovery access.

## Hardware validation checklist

After installation, verify cold boot, recovery AP, web setup, MQTT playback,
sound upload, volume, repeated playback and OTA. Test interrupted-update
rollback while preserving UART recovery access. Record the board revision
and TI service-pack version when reporting results.
