# Line Input RTP

This ACAP sends the C8110 line input as unicast G.711 A-law RTP while the input stays above a level threshold.

## Prerequisites

- AXIS C8110, or another AXIS OS audio device with PipeWire
- AXIS OS 12.11.72 through 13. The C8110 is `armv7hf` (1.4.1 was tested on one running 12.11.77). An `aarch64` build is published for ARTPEC-8 and ARTPEC-9 devices.
- Docker, for the ACAP Native SDK images `axisecp/acap-native-sdk:12.11.0-<arch>-ubuntu24.04`

## How it works

PipeWire captures `AudioDevice0Input0.Unprocessed`. The app measures the input peak in dBFS. When it reaches the activation level, the app opens a gate after a short activation delay and holds it through brief gaps. While the gate is open it sends 20 ms G.711 A-law packets to the configured IPv4 address and UDP port. When the gate closes, packets stop.

The capture is mixed to mono and passed through a 4 kHz low-pass filter before it is reduced to 8 kHz, so content above the telephone band doesn't fold back into it. The first packets after the gate opens carry up to 200 ms of audio from before it opened (the activation delay), so the start of the sound that triggered it isn't lost. The RTP timestamp keeps counting through quiet gaps, as RFC 3551 expects for silence suppression, and a failed send counts as a lost packet.

The activation level is this app's own setting, `ThresholdDb`, from −90 to 0 dBFS in 1 dB steps. The gate closes once the peak drops below the activation level minus `MarginDb`. That closing level never goes below −90 dBFS, which is where silence on the C8110 sits. Before 1.6.0 the activation level followed the device's Audio detection setting, a whole number from 0 to 100 on a linear scale. That setting couldn't go below about −40 dBFS, so quiet microphone sources never triggered.

Parameter reads and writes run on a separate control thread, so a save never stalls audio capture. If the PipeWire stream fails, or delivers no audio for 5 seconds, the app rebuilds it with a backoff of 1 to 30 seconds. If the PipeWire daemon goes away, the app exits so AXIS OS respawns it.

## Settings page

The page at `/local/lineinrtp/` has three parts:

- **Live chart.** The last 24 seconds of input peak level in dBFS, from −90 to 0 dB, so quiet and loud sources are both readable. Lines mark where the gate opens and closes, and a timeline below shows what was actually sent. Dark green is opening, with a caret where the trigger started. Light green is sending. Amber is holding: still sending while the release hold counts down. Purple is the max-on limit re-arming. The bars use the same time scale and width as the graph at any page size. In I/O-port mode the chart shows the port state instead.
- **Input settings and hints.** The page reads the device's audio input type and gain (`AudioSource.A0.InputType` and `InputGain`) and shows them beside the chart. It shows a hint for a faint signal (loudest peak between −75 and −45 dBFS over 10 s) or a clipping one, worded for the current input type. Silence on the C8110 sits near −90 dBFS, so a quiet room doesn't trigger the hint.
- **Preview.** Moving any setting replays the recorded input with the unsaved values and shows the result as a second timeline, before you save.
- **One Mode control.** Off, Auto, Always on, and Test tone replace the separate Enabled, Force on, and Test mode switches. The trigger choice (audio level, I/O port 1, I/O port 2) shows only in Auto.

The activation level slider runs from −90 to 0 dBFS. The release margin slider runs from 0 to 40 dB, but its top end shrinks as the activation level nears −90, so the closing level stays at or above the floor. Lowering the activation level pulls the margin down with it when needed. "Saved" only appears after the app has written the values and read them back.

## Build

```bash
for ARCH in armv7hf aarch64; do
  docker build --platform linux/amd64 --build-arg ARCH=$ARCH -t lineinrtp:1.7.0-$ARCH .
  id=$(docker create lineinrtp:1.7.0-$ARCH)
  docker cp "$id":/opt/app/Line_Input_RTP_1_7_0_$ARCH.eap .
  docker rm "$id"
done
```

`acap-build` names each package from the friendly name, for example `Line_Input_RTP_1_7_0_armv7hf.eap`.

## Working on the settings page

`tools/mock_device.js` serves the page with a fake device behind it, so you can work on the UI without a camera. It needs Node.js only:

```bash
node tools/mock_device.js
```

Then open http://localhost:8099/local/lineinrtp/. Options simulate a slow or unreliable device: `--latency` (ms per reply), `--save-ms` (ms per changed setting before a save is confirmed), `--fail` and `--hang` (fraction of status requests that fail or never answer). The same knobs can be changed while it runs with `curl -X POST localhost:8099/mock/control -d '{"hang":1}'`.

## Tests

The pure logic (A-law encoder, address and node-name validation, and the gate state machine) lives in `app/rtpcore.c`, with no PipeWire or AXIS SDK dependencies, so it builds and tests on any Linux or macOS host:

```bash
make -C tests            # build and run
make -C tests SAN=1      # with AddressSanitizer and UBSan
python3 -m unittest discover -s tests
```

GitHub Actions runs these on every push and pull request, with gcc and clang, and builds unsigned `.eap` packages for `armv7hf` and `aarch64`. The packages are attached to each run as artifacts.

## Signing

Release packages are signed with the Axis ACAP Signing Service (https://www.axis.com/support/acap-signing). The service only signs a package whose `vendor` and `vendorId` match the signing account, so `app/manifest.json` carries that account's values. Signing appends a PGP signature to the unsigned `.eap`. Sign each architecture's package separately, and rebuild and re-sign for every release.

## Install

Signed releases install on AXIS OS 12 and 13 without changing any device setting:

```bash
curl --anyauth -u "$USER:$PASS" -F packfil=@signed_Line_Input_RTP_1_7_0_armv7hf.eap \
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
| DestinationIp | empty | Unicast IPv4 address of the RTP listener. Multicast, 240.0.0.0/4 (including 255.255.255.255), loopback 127.0.0.0/8, and 0.0.0.0/8 are rejected |
| DestinationPort | 49000 | UDP port |
| ThresholdDb | -45 | Activation level in dBFS peak, −90 to 0 |
| MarginDb | 6 | Close the gate this many dB below the activation level, 0 to 40. The closing level never goes below −90 dBFS |
| ActivationDelayMs | 200 | How long the trigger must stay active |
| DeactivationDelayS | 2 | How long it must stay quiet |
| MaxOnS | 0 | Force off after this many seconds. 0 disables the limit |
| InputNode | AudioDevice0Input0.Unprocessed | PipeWire source |

`GET /local/lineinrtp/api/status` returns level, gate, packet counts, and the last error. It also returns:

- `history`: up to 25.6 s of 100 ms entries of peak level (0–100 linear, fractional), gate code, and port states. Gate codes: 0 idle, 1 opening, 2 sending, 3 holding, 4 max-on re-arming, 5 test tone. `history.seq` is one past the newest entry's sequence number. Pass `?since=<sequence number>` to get only newer entries; the settings page does this on every poll.
- `configRev` and `configError`.

`POST /local/lineinrtp/api/config` saves the fields above. The reply carries a `rev`, and the save is complete once `/api/status` reports `configRev` at or above that value. Status is viewer. Configuration is admin.

## Known limitations

- Codec is G.711 A-law only. It is intelligible and matches a static RTP payload type. It is not a music codec.
- The destination has to be a unicast IPv4 address. The app will not send multicast.
- Changing the input node reconnects when that PipeWire node is already known. A node that has not appeared yet is picked up on the next registry event.
- `AUDIT.md` lists the findings from the 1.5.0 review and which ones are still open.

## License

MIT. See `LICENSE`.
