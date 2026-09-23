# OpenCam — Milestone 1 Technical Report

**Project:** OpenCam  
**Base project:** VCamdroid (`darusc/VCamdroid`)  
**Platform:** Android → Linux → V4L2 → OBS  
**Milestone:** 1 — End-to-End Camera Streaming Proof of Concept  
**Status:** ✅ COMPLETE  
**Date:** 20 September 2026

---

## 1. Milestone Objective

The objective of Milestone 1 was to prove that an Android phone camera can be used as a live webcam on Linux through the existing VCamdroid Android implementation, without porting the Windows GUI application.

The target pipeline was:

```text
Android Camera
      ↓
VCamdroid RTSP Server
      ↓ Wi-Fi
Linux
      ↓
FFmpeg
      ↓
V4L2 Loopback
      ↓
/dev/video2
      ↓
OBS
```

The milestone is considered complete because the phone camera was successfully displayed live in OBS as a Linux virtual camera.

---

# 2. Final Working Architecture

The working system consists of two communication paths.

### Control path

```text
Android
   │
   │ TCP :6969
   ▼
Linux Controller
```

The Linux side sends an activation command over this connection.

### Video path

```text
Android Camera
   │
   │ RTSP :8554
   ▼
Linux / FFmpeg
   │
   │ decoded YUV420p
   ▼
V4L2 Loopback
   │
   ▼
/dev/video2
   │
   ▼
OBS
```

The control connection is required because the Android RTSP server does not start merely by opening the application.

---

# 3. Important Discovery: Android Startup Flow

The Android application does not provide a traditional menu.

When launched, it immediately presents the QR scanner.

The QR code contains:

```text
IP_ADDRESS:PORT
```

For example:

```text
10.196.6.197:6969
```

The Android application parses this value and establishes a TCP connection to the specified controller.

After connecting, Android sends a `DeviceDescriptor`.

The Linux controller then sends an `ACTIVATION` packet.

Only after receiving that activation packet does Android start its RTSP server.

---

# 4. QR Protocol

The QR format was confirmed from:

```text
android/app/src/main/java/com/darusc/vcamdroid/QRScanner.kt
```

The parser performs:

```text
value.split(":")
```

and expects exactly two components:

```text
IP address
port
```

Therefore the QR payload is simply:

```text
10.196.6.197:6969
```

No JSON or additional encoding is required.

---

# 5. Network Setup Used During Testing

Initially, the phone and Linux machine were connected to the same SSID but placed on different subnets:

```text
Linux: 192.168.0.104
Phone: 192.168.1.113
```

They could not communicate directly.

For testing, the phone's mobile hotspot was used.

The resulting network was:

```text
Linux:  10.196.6.197
Phone:  10.196.6.129
Network: 10.196.6.0/24
```

Connectivity was verified with:

```bash
ping -c 4 10.196.6.129
```

Result:

```text
0% packet loss
```

This established reliable LAN connectivity.

---

# 6. ADB Setup

ADB was installed on Arch Linux using:

```bash
sudo pacman -S android-tools
```

ADB version:

```text
Android Debug Bridge version 1.0.41
Version 37.0.0-android-tools
```

The Android device was successfully detected:

```text
List of devices attached
RZGL125GYWV    device
```

ADB was mainly useful for discovering the phone's IP address:

```bash
adb shell ip route
```

Example result during the hotspot test:

```text
10.196.6.0/24 dev swlan0 proto kernel scope link src 10.196.6.129
```

USB/ADB was not used for the final Wi-Fi streaming path.

---

# 7. Device Descriptor Protocol

After the Android application connects to TCP port `6969`, it sends a binary device descriptor.

During the successful test:

```text
Received descriptor: 483 bytes
```

The descriptor contained information including:

```text
Device model:
SM-M366B

RTSP URL:
rtsp://10.196.6.129:8554/live
```

The descriptor format was confirmed from:

```text
android/.../networking/DeviceDescriptor.kt
windows/src/net/serializer.cpp
```

Important properties:

- Big-endian integers
- Strings preceded by uint16 length
- Resolution counts encoded as uint16
- Resolution dimensions encoded as uint16 pairs
- Filter information included in the descriptor

---

# 8. Activation Protocol

The Android activation packet is identified by:

```text
ACTIVATION = 0x02
```

The Android implementation parses the packet in:

```text
android/app/src/main/java/com/darusc/vcamdroid/rtsp/StreamOptions.kt
```

The packet used for the successful test contained:

```text
Packet type:       0x02
FPS:               30
Width:             640
Height:            480
Camera:            BACK
Adaptive bitrate:  false
Bitrate:           4096000
Min bitrate:       512000
Max bitrate:       25600000
Stabilization:     false
Flash:             false
H265:              false
Focus mode:        0
Filter count:      0
Active effect:     ""
```

The resulting activation packet was:

```text
41 bytes
```

The Linux test controller successfully sent this packet.

---

# 9. Important Protocol Inconsistency

A significant issue was discovered between the current Windows serializer and Android deserializer.

### Windows serializer

`windows/src/net/serializer.cpp`

writes boolean values using:

```cpp
WriteInt32(buffer, value);
```

Therefore booleans occupy **4 bytes**.

### Android deserializer

`StreamOptions.kt` reads some boolean fields using:

```kotlin
buffer.get()
```

Therefore those fields occupy **1 byte**.

Android also expects:

```text
focusMode : int32
```

while the current Windows serializer does not serialize focus mode.

Therefore the current Windows serializer and Android deserializer are not byte-for-byte compatible in these fields.

For Milestone 1, the Linux test controller therefore used the **Android deserializer's actual expected format**, rather than blindly copying the Windows serializer.

This should be investigated before implementing the permanent Linux protocol layer.

---

# 10. Temporary Linux Activation Server

A temporary Python TCP server was created for Milestone 1.

Its responsibilities were intentionally minimal:

1. Listen on TCP `6969`
2. Accept Android connection
3. Receive the device descriptor
4. Construct the activation packet
5. Send activation
6. Keep the TCP connection alive

The server successfully produced:

```text
Android connected from ('10.196.6.129', 51306)

Received descriptor: 483 bytes
Sending ACTIVATION...
Activation sent. Keeping connection open.
```

This proved that a Linux controller can replace the Windows application for the purposes of controlling the existing Android implementation.

This server is **temporary test code**, not the final OpenCam CLI.

---

# 11. RTSP Server Verification

After activation, the Android RTSP server became available at:

```text
rtsp://10.196.6.129:8554/live
```

The stream was verified using:

```bash
ffprobe -v error -show_streams rtsp://10.196.6.129:8554/live
```

The video stream reported:

```text
Codec: H.264
Profile: High
Resolution: 640x480
Pixel format: yuv420p
Average FPS: 30
```

The stream also contained:

```text
Codec: AAC
Sample rate: 32000 Hz
Channels: 2
```

Therefore the Android device successfully produced a real camera RTSP stream.

---

# 12. V4L2 Loopback

The V4L2 loopback module was already installed but disappeared after reboot because it was not loaded.

Installed module:

```text
v4l2loopback 0.15.4
```

The module was loaded with:

```bash
sudo modprobe v4l2loopback \
  devices=1 \
  video_nr=2 \
  card_label="OpenCam" \
  exclusive_caps=1
```

This created:

```text
/dev/video2
```

The physical cameras remained on `/dev/video0` and `/dev/video1`.

---

# 13. FFmpeg Pipeline

The successful pipeline was:

```bash
ffmpeg -hide_banner \
  -rtsp_transport tcp \
  -i rtsp://10.196.6.129:8554/live \
  -an \
  -vf format=yuv420p \
  -f v4l2 \
  -pix_fmt yuv420p \
  -video_size 640x480 \
  /dev/video2
```

The pipeline performs:

```text
RTSP
 ↓
H.264 decoder
 ↓
Raw YUV420p
 ↓
V4L2 output
 ↓
/dev/video2
```

Audio was intentionally discarded using:

```text
-an
```

because the immediate milestone only requires a video webcam.

---

# 14. OBS Verification

OBS successfully opened:

```text
/dev/video2
```

and displayed the live Android camera feed.

The observed stream was:

- Smooth
- Stable
- Usable as a webcam
- Approximately 2–4 seconds of end-to-end latency

The latency is currently considered acceptable for the first MVP proof of concept.

Latency optimization was deliberately deferred.

---

# 15. Milestone 1 Definition of Done

All required conditions were satisfied:

```text
[✓] Android camera works
[✓] Android connects to Linux over Wi-Fi
[✓] QR pairing works
[✓] Linux receives DeviceDescriptor
[✓] Linux sends activation packet
[✓] Android starts RTSP server
[✓] Linux receives H.264 RTSP stream
[✓] FFmpeg decodes stream
[✓] V4L2 loopback receives frames
[✓] /dev/video2 exists
[✓] OBS receives live camera
```

### Final result

**Phone camera → OpenCam Linux pipeline → OBS**

is working end-to-end.

---

# 16. What Milestone 1 Proved

The most important architectural conclusion is:

> The Windows VCamdroid application does not need to be ported to Linux.

The Android application can communicate with a Linux controller as long as the controller implements the required TCP protocol.

This allows OpenCam to evolve into a native Linux CLI/controller while reusing the existing Android streaming implementation.

---

# 17. Milestone 2 Direction

Milestone 2 should turn the temporary proof-of-concept controller into the beginning of the actual **OpenCam Linux CLI**.

The intended architecture is:

```text
                  OpenCam CLI
                       │
          ┌────────────┴────────────┐
          │                         │
    Control Server              Pipeline
       TCP :6969                   │
          │                        │
          ▼                        ▼
     Android app              FFmpeg
          │                        │
          │ RTSP :8554             │
          └──────────────►─────────┘
                                   │
                                   ▼
                              /dev/video2
                                   │
                                   ▼
                                  OBS
```

Potential Milestone 2 components:

### Control layer

- TCP server
- Android connection handling
- Device descriptor parser
- Protocol serializer
- Activation command
- Connection lifecycle
- Device discovery/pairing
- QR generation

### Streaming layer

- RTSP URL extraction
- FFmpeg process management
- Decode configuration
- V4L2 output
- Resolution configuration
- Codec configuration
- Error handling

### CLI layer

Possible commands:

```text
opencam start
opencam devices
opencam connect
opencam stop
opencam status
```

Exact CLI design should be decided during Milestone 2 rather than prematurely implemented.

---

# 18. Important Technical Debt for Milestone 2

Before finalizing the protocol implementation, investigate:

1. **Windows serializer vs Android deserializer mismatch**
   - Boolean field widths differ.
   - Android expects focus mode.
   - Windows serializer currently omits focus mode.

2. **TCP packet framing**
   - Current implementation essentially assumes packet boundaries from socket reads.
   - A robust Linux implementation should account for TCP fragmentation/coalescing.

3. **Device descriptor parsing**
   - Implement a proper parser rather than relying on the temporary server.

4. **FFmpeg lifecycle**
   - Start/stop cleanly.
   - Detect crashes.
   - Handle RTSP disconnects.

5. **V4L2 loopback lifecycle**
   - Automatically detect or create the virtual camera.
   - Consider persistent module configuration.

6. **Latency**
   - Current observed latency: approximately 2–4 seconds.
   - Optimization can be investigated after the basic CLI is stable.

7. **Audio**
   - Current MVP discards AAC audio.
   - Decide later whether OpenCam should expose audio separately.

---

# 19. Current Known Working Addresses

These addresses were specific to the temporary hotspot network and should **not** be hard-coded into OpenCam.

During testing:

```text
Linux controller:
10.196.6.197:6969

Android:
10.196.6.129:8554

RTSP:
rtsp://10.196.6.129:8554/live

Virtual camera:
/dev/video2
```

These will change depending on the network.

---

# 20. Milestone 2 Starting Point

When beginning Milestone 2, start from this assumption:

> **The Android application already works. Do not modify its UI or streaming architecture unless required by a discovered protocol issue.**

The immediate goal should be to replace the temporary Python activation server with a proper Linux-side controller.

The first useful Milestone 2 implementation should be able to:

```text
1. Start OpenCam controller
2. Listen on TCP 6969
3. Accept Android connection
4. Parse DeviceDescriptor
5. Display detected Android device
6. Send correct ACTIVATION packet
7. Extract RTSP URL
8. Start FFmpeg
9. Create/use /dev/video2
10. Provide a working webcam to Linux/OBS
```

Only after that baseline works should additional CLI features be introduced.

---

# 21. Final Milestone 1 Conclusion

**OpenCam's fundamental concept has been experimentally validated.**

A stock Android VCamdroid client can be controlled from Linux without the original Windows application.

The complete camera path:

```text
Android Camera
      ↓
Android RTSP Server
      ↓
Wi-Fi
      ↓
Linux
      ↓
FFmpeg
      ↓
V4L2 Loopback
      ↓
/dev/video2
      ↓
OBS
```

has been demonstrated successfully.

**Milestone 1: COMPLETE ✅**

**Next:** Milestone 2 — build the real OpenCam Linux CLI/controller around the protocol and streaming pipeline discovered here.
