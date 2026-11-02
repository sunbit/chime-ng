# MQTT 5 / Homie 5 contract

The broker must support MQTT 5.0 and QoS 2. The root is `homie/5/<id>`,
where `<id>` is the stable ID entered in the web configurator. The client ID
is generated from the complete device MAC. Description version: **6**.

| Suffix below root | Direction | Payload | QoS | Retained |
| --- | --- | --- | ---: | --- |
| `$description` | Device to controller | Homie 5 JSON | 2 | Yes |
| `$state` | Device / last will to controller | `init`, `ready`, `lost`, `disconnected` | 2 | Yes |
| `chime/play/set` | Controller to device | `original` | 0 | No |
| `chime/play` | Device event | `original` | 0 | No |
| `chime/playing` | Device state | `true` / `false` | 2 | Yes |
| `chime/volume/set` | Controller to device | Integer `0` through `100` | 0 | No |
| `chime/volume` | Device state | Volume percentage | 2 | Yes |
| `chime/led/set` | Controller to device | `true` / `false` | 0 | No |
| `chime/led` | Device state | Requested LED state | 2 | Yes |
| `chime/led-diagnostic` | Device state | GPIO latch and pad configuration | 2 | Yes |
| `chime/led-command` | Device state | Last MQTT LED command result | 2 | Yes |
| `chime/error` | Device state | Audio error enum | 2 | Yes |
| `chime/diagnostic` | Device state | I2C/audio/DMA diagnostic | 2 | Yes |
| `system/ip` | Device state | Current IPv4 address | 2 | Yes |
| `system/web` | Device state | `listening:80`, `stopped`, or failing stage | 2 | Yes |
| `system/firmware` | Device state | Running version | 2 | Yes |
| `system/uptime` | Device state | Seconds since boot, every 30 seconds | 2 | Yes |

The `original` command plays the currently selected sound. Initial
installation uses the bundled synthesized WAV. Upload a replacement through
the web interface; firmware OTA preserves the selected sound.

## Command examples

For an example ID of `entrance-chime`, with your broker and credentials:

```sh
mosquitto_pub -V mqttv5 -h YOUR_BROKER -q 0 \
  -t 'homie/5/entrance-chime/chime/play/set' -m original
mosquitto_pub -V mqttv5 -h YOUR_BROKER -q 0 \
  -t 'homie/5/entrance-chime/chime/volume/set' -m 35
mosquitto_pub -V mqttv5 -h YOUR_BROKER -q 0 \
  -t 'homie/5/entrance-chime/chime/led/set' -m true
```

Do not enable retain on commands. Commands must use the exact plain-text
payload, without JSON quotes, whitespace or a trailing newline. **Publish to
`/set`**: writing to `chime/led` changes the broker's displayed state but does
not invoke the device command handler.

## Lifecycle and command rules

The device starts with a clean MQTT session and retained QoS 2 last will
`lost`. It publishes `init`, then `$description`, completes all three command
subscriptions, publishes current state and enters `ready` only when audio is
usable. A codec/WAV error can keep MQTT in `init`; IP, diagnostic and web/OTA
remain available. Retain Handling 2 and Retain As Published 1 prevent replay
of old retained commands and identify incorrectly retained live commands.

All MQTT commands require `ready` and are rejected during OTA or pending
restart. RETAIN, DUP and unsupported QoS are rejected. Playback rejects a
second command while already playing. Volume changes require idle playback
and are persisted before publication. LED changes can occur during sound and
cancel the three-second startup timer. LED state is volatile across reboot.

`led-command` starts at `seq=0 result=none`. Each decoded PUBLISH to the
exact LED command topic increments the sequence, including rejected commands.
It records the result, QoS, RETAIN, DUP, payload length and up to eight payload
bytes in hex. Results include `applied-true`, `applied-false`, `rejected-qos`,
`rejected-not-ready`, `rejected-busy`, `rejected-retained`,
`rejected-duplicate` and `rejected-payload`. Only the latest command is kept.
Web LED controls do not increment this MQTT counter. GPIO `latch` and `pad`
readings do not establish visible light or physical pin voltage.

The audio error enum is
`none,codec,wav,underrun,storage,dma-timeout,audio-timeout,dma-error,i2s-error,i2s-start`.
`playing` describes the software engine, not an acoustic measurement.

One QoS 2 publication is in flight at a time. Missing acknowledgements trigger
reconnection; commands and playback impulses are not replayed. Keepalive is
30 seconds and reconnect delay is five seconds. An orderly shutdown attempts
retained `disconnected` and DISCONNECT; otherwise closing the connection lets
the broker publish the last will.

Uptime wraps with the 32-bit millisecond clock at about 49.7 days. It is not
a timestamp. Renaming the Homie ID creates a new topic tree; old retained
topics are not automatically purged. Home Assistant discovery is not built
in: integrate the documented topics explicitly with your controller.
