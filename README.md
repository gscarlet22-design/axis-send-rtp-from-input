# Line Input RTP

This ACAP sends the C8110 line input as unicast G.711 A-law RTP while the input stays above a level threshold.

## Prerequisites

- AXIS C8110, or another AXIS OS audio device with PipeWire
- AXIS OS 12.11.72 through 13 (1.4.1 was tested on a C8110 running 12.11.77, `armv7hf`)
- Docker, for the ACAP Native SDK image `axisecp/acap-native-sdk:12.11.0-armv7hf-ubuntu24.04`

## How it works

PipeWire captures `AudioDevice0Input0.Unprocessed`. The app measures the input peak on a 0–100 scale. When it reaches the device's Audio detection level, the app opens a gate after a short activation delay and holds it through brief gaps. While the gate is open it sends 20 ms G.711 A-law packets to the configured IPv4 address and UDP port. When the gate closes, packets stop.

The activation level is the device's own Audio detection setting (`AudioSource.A0.AlarmLevel`). The app reads it over VAPIX every 5 seconds, using the ACAP's VAPIX service account, so the level trigger works without anyone opening the settings page. If that read fails, the app falls back to the last known value, which the settings page also reports.

Parameter reads and writes run on a separate control thread, so a save never stalls audio capture. If the PipeWire stream fails, or delivers no audio for 5 seconds, the app rebuilds it with a backoff of 1 to 30 seconds. If the PipeWire daemon goes away, the app exits so AXIS OS respawns it.

## Settings page

The page at `/local/lineinrtp/` has three parts:

- **Live chart.** The last 24 seconds of input level, with lines where the gate opens and closes, and a timeline of what was actually sent. Green is sending, yellow is opening or holding, red is the max-on limit re-arming. In I/O-port mode the chart shows the port state instead.
- **Preview.** Moving any setting replays the recorded input with the unsaved values and shows the result as a second timeline, before you save.
- **One Mode control.** Off, Auto, Always on, and Test tone replace the separate Enabled, Force on, and Test mode switches. The trigger choice (audio level, I/O port 1, I/O port 2) shows only in Auto.

The activation level slider writes the device's Audio detection setting, so it can be set from either page. "Saved" only appears after the app has written the values and read them back.

## Build

```bash
docker build --platform linux/amd64 --build-arg ARCH=armv7hf -t lineinrtp:1.5.0 .
id=$(docker create lineinrtp:1.5.0)
docker cp "$id":/opt/app/Line_Input_RTP_1_5_0_armv7hf.eap .
docker rm "$id"
```

`acap-build` names the package from the friendly name: `Line_Input_RTP_1_5_0_armv7hf.eap`.

## Signing

Release packages are signed with the Axis ACAP Signing Service (https://www.axis.com/support/acap-signing). The service only signs a package whose `vendor` and `vendorId` match the signing account, so `app/manifest.json` carries that account's values. Signing appends a PGP signature to the unsigned `.eap`. Rebuild and re-sign for every release.

## Install

Signed releases install on AXIS OS 12 and 13 without changing any device setting:

```bash
curl --anyauth -u "$USER:$PASS" -F packfil=@signed_Line_Input_RTP_1_5_0_armv7hf.eap \
  "https://$DEVICE/axis-cgi/applications/upload.cgi"
```

An unsigned local build needs unsigned apps allowed first. On AXIS OS 12:

```bash
curl --anyauth -u "$USER:$PASS" -X POST \
  "http://$DEVICE/axis-cgi/applications/config.cgi?action=set&name=AllowUnsigned&value=true"
```

Open the app from the device's Apps page. The page is served at `/local/lineinrtp/`.

## Audio Manager Pro

Create an RTP source and leave multicast off.

- Codec: G.711a, mono, 8 kHz
- Payload type: 8
- Listening port: the same port configured on the app (default 49000)
- Optional source IP: the C8110

Set the app's destination to the Audio Manager Pro server address. The settings page shows that target as `rtp://<address>:<port>` with a Copy button. The server sees packets only while the gate is open, in **Always on** mode, or in **Test tone** mode.

Set the jack to line input on the device Audio page before trusting the threshold. Silence on this C8110 sits near −90 dBFS.

## Configuration

| Parameter | Default | Meaning |
| --- | --- | --- |
| Enabled | no | Master switch. Off in the Mode control |
| ForceOn | no | Send the line input even when it is quiet. Always on in the Mode control |
| TestMode | no | Send a 1 kHz test tone instead of the line input. Test tone in the Mode control |
| Trigger | level | `level` (default), `port1`, or `port2`. A port uses that input instead of the audio level |
| DestinationIp | empty | Unicast IPv4 address of the RTP listener. Multicast, 240.0.0.0/4, and 0.0.0.0/8 are rejected |
| DestinationPort | 49000 | UDP port |
| ThresholdDb | unused | Activation level is the device Audio detection setting, 0–100 |
| MarginDb | 6 | Close the gate this many dB below the activation level |
| ActivationDelayMs | 200 | How long the trigger must stay active |
| DeactivationDelayS | 2 | How long it must stay quiet |
| MaxOnS | 0 | Force off after this many seconds. 0 disables the limit |
| InputNode | AudioDevice0Input0.Unprocessed | PipeWire source |

`GET /local/lineinrtp/api/status` returns level, gate, packet counts, and the last error. It also returns:

- `history`: 100 ms entries of peak level, gate code, and port states. Gate codes: 0 idle, 1 opening, 2 sending, 3 holding, 4 max-on re-arming, 5 test tone.
- `alarmSource`: `device`, `page`, `cached`, or `none`.
- `configRev` and `configError`.

`POST /local/lineinrtp/api/config` saves the fields above. The reply carries a `rev`, and the save is complete once `/api/status` reports `configRev` at or above that value. Status is viewer. Configuration is admin.

## Known limitations

- Codec is G.711 A-law only. It is intelligible and matches a static RTP payload type. It is not a music codec.
- The destination has to be a unicast IPv4 address. The app will not send multicast.
- Changing the input node reconnects when that PipeWire node is already known. A node that has not appeared yet is picked up on the next registry event.
- `AUDIT.md` lists the remaining medium and low findings from the 1.5.0 review.

## License

MIT. See `LICENSE`.
