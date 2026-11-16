# Line Input RTP

This ACAP sends the C8110 line input as unicast G.711 A-law RTP while the input stays above a level threshold.

## Prerequisites

- AXIS C8110, or another AXIS OS audio device with PipeWire
- AXIS OS 12.11.72 through 12.x (tested on a C8110 running 12.11.77, `armv7hf`)
- Docker, for the ACAP Native SDK image `axisecp/acap-native-sdk:12.11.0-armv7hf-ubuntu24.04`

## How it works

PipeWire captures `AudioDevice0Input0.Unprocessed`. The app measures level in dBFS, opens a gate after a short attack, and holds it through brief gaps. While the gate is open it sends 20 ms G.711 A-law packets to the configured IPv4 address and UDP port. When the gate closes, packets stop.

## Build

```bash
docker build --platform linux/amd64 --build-arg ARCH=armv7hf -t lineinrtp:1.4.1 .
id=$(docker create --name lineinrtp-tmp lineinrtp:1.4.1)
docker cp "$id":/opt/app/Line_Input_RTP_1_4_1_armv7hf.eap .
docker rm lineinrtp-tmp
```

`acap-build` names the package from the friendly name: `Line_Input_RTP_1_4_1_armv7hf.eap`.

## Install

On AXIS OS 12 the device rejects unsigned packages until unsigned apps are allowed. On a lab device:

```bash
curl --anyauth -u "$USER:$PASS" -X POST \
  "http://$DEVICE/axis-cgi/applications/config.cgi?action=set&name=AllowUnsigned&value=true"
curl --anyauth -u "$USER:$PASS" -F packfil=@Line_Input_RTP_1_4_1_armv7hf.eap \
  "http://$DEVICE/axis-cgi/applications/upload.cgi"
```

Open the app from the device's Apps page. The page is served at `/local/lineinrtp/`.

## Audio Manager Pro

Create an RTP source and leave multicast off.

- Codec: G.711a, mono, 8 kHz
- Payload type: 8
- Listening port: the same port configured on the app (default 49000)
- Optional source IP: the C8110

Set the app's destination to the Audio Manager Pro server address. The settings page shows that target as `rtp://<address>:<port>` with a Copy button. The server sees packets only while the gate is active, while **Send while quiet** is on, or while **Test mode** is sending its 1 kHz tone.

Set the jack to line input on the device Audio page before trusting the threshold. Silence on this C8110 sits near −90 dBFS.

## Configuration

| Parameter | Default | Meaning |
| --- | --- | --- |
| Enabled | no | Master switch |
| ForceOn | no | Send the line input even when it is quiet |
| TestMode | no | Send a 1 kHz test tone instead of the line input |
| Trigger | level | `level` (default), `port1`, or `port2`. A port uses that input instead of the audio level |
| DestinationIp | empty | Unicast IPv4 address of the RTP listener. Multicast, broadcast, and 0.0.0.0/8 are rejected |
| DestinationPort | 49000 | UDP port |
| ThresholdDb | unused | Activation level is the device Audio detection slider, 0–100 |
| MarginDb | 6 | Close the gate this many dB below the threshold |
| ActivationDelayMs | 200 | How long the level must stay hot |
| DeactivationDelayS | 2 | How long it must stay quiet |
| MaxOnS | 0 | Force off after this many seconds. 0 disables the limit |
| InputNode | AudioDevice0Input0.Unprocessed | PipeWire source |

`GET /local/lineinrtp/api/status` returns level, gate, packet counts, and the last error. `POST /local/lineinrtp/api/config` saves the fields above. Status is viewer. Configuration is admin.

## Known limitations

- Codec is G.711 A-law only. It is intelligible and matches a static RTP payload type. It is not a music codec.
- The destination has to be a unicast IPv4 address. The app will not send multicast or broadcast.
- The app does not sign itself. AXIS OS 13 may remove the unsigned-app toggle.
- Changing the input node reconnects when that PipeWire node is already known. A node that has not appeared yet is picked up on the next registry event.

## License

MIT. See `LICENSE`.
