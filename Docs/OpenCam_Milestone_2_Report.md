# OpenCam --- Milestone 2 Technical Report

**Milestone:** M2 --- OpenCam Linux Controller\
**Status:** Complete / Frozen\
**Platform:** Arch Linux\
**Project:** OpenCam\
**Baseline:** Milestone 1 Android → Linux RTSP video pipeline

------------------------------------------------------------------------

## 1. Milestone Overview

Milestone 1 proved that the VCamdroid Android application could provide
a live camera stream to Linux through RTSP, where FFmpeg could decode
the stream and write it into a V4L2 loopback device for consumption by
applications such as OBS.

Milestone 2 converted that experimentally verified pipeline into a small
native Linux controller written in C++.

The controller now handles the core lifecycle:

``` text
OpenCam Linux Controller
        │
        ├── generates pairing QR
        │
        ├── accepts Android TCP connection
        │
        ├── receives DeviceDescriptor
        │
        ├── discovers/loads OpenCam V4L2 device
        │
        ├── sends Android activation packet
        │
        ├── waits for RTSP server
        │
        ├── launches FFmpeg
        │
        ├── bridges video → V4L2
        │
        └── cleans up on disconnect
```

The milestone also introduced dynamic pairing, dynamic V4L2 device
discovery, reconnect handling, and a basic CMake-based C++ project
structure.

------------------------------------------------------------------------

# 2. Milestone Goals

The practical goals for M2 were:

-   Replace the temporary Python controller with a native Linux
    implementation.
-   Implement the VCamdroid TCP control protocol required to start the
    Android stream.
-   Parse the Android `DeviceDescriptor`.
-   Construct the correct Android activation packet.
-   Dynamically determine the Linux machine's IPv4 address.
-   Generate a QR code that Android can scan.
-   Dynamically locate the V4L2 loopback device instead of assuming
    `/dev/video2`.
-   Launch and stop FFmpeg from the controller.
-   Detect when the Android RTSP server becomes available.
-   Keep the controller alive after Android disconnects.
-   Allow another Android connection without restarting OpenCam.
-   Preserve the working M1 video path through OBS and browser webcam
    capture.

------------------------------------------------------------------------

# 3. Final M2 Architecture

The resulting architecture is:

``` text
                    ┌──────────────────────┐
                    │   Android / VCamdroid│
                    │                      │
                    │ Camera               │
                    │ RTSP Server :8554    │
                    └──────────┬───────────┘
                               │
                         RTSP over Wi-Fi
                               │
                               ▼
┌───────────────────────────────────────────────────────┐
│                    Linux / OpenCam                    │
│                                                       │
│  TCP Control :6969                                    │
│       ▲                                               │
│       │ DeviceDescriptor / Activation                │
│       │                                               │
│  ┌────┴───────────────┐                               │
│  │ OpenCam Controller │                               │
│  └────┬───────────────┘                               │
│       │                                               │
│       ├── QR pairing                                  │
│       ├── protocol handling                           │
│       ├── RTSP readiness check                        │
│       ├── FFmpeg process management                   │
│       └── V4L2 device discovery                      │
│                                                       │
│                    FFmpeg                             │
│                       │                               │
│                       ▼                               │
│                 V4L2 loopback                        │
│                       │                               │
│                       ▼                               │
│                  /dev/video*                          │
└───────────────────────┬───────────────────────────────┘
                        │
                        ▼
                  OBS / Browser /
                  Webcam Applications
```

The Linux implementation does not depend on `/dev/video2` specifically.
It searches for the V4L2 device whose kernel-reported name is `OpenCam`.

------------------------------------------------------------------------

# 4. Project Structure

The Linux controller was organized as a small CMake project:

``` text
linux/
├── CMakeLists.txt
├── src/
│   ├── main.cpp
│   ├── device_descriptor.hpp
│   ├── device_descriptor.cpp
│   ├── stream_options.hpp
│   ├── stream_options.cpp
│   ├── ffmpeg_runner.hpp
│   ├── ffmpeg_runner.cpp
│   ├── rtsp_probe.hpp
│   ├── rtsp_probe.cpp
│   ├── v4l2_device.hpp
│   ├── v4l2_device.cpp
│   ├── network.hpp
│   ├── network.cpp
│   ├── qr.hpp
│   └── qr.cpp
└── build/
    └── opencam
```

C++17 is used as the project language standard.

------------------------------------------------------------------------

# 5. Android Control Protocol Implementation

## 5.1 TCP Control Connection

The Android application connects to the Linux controller over TCP port
`6969`.

The Linux controller:

1.  Creates a TCP socket.
2.  Binds to `0.0.0.0:6969`.
3.  Enables `SO_REUSEADDR`.
4.  Listens for incoming connections.
5.  Accepts Android connections.
6.  Receives the `DeviceDescriptor`.
7.  Sends the activation packet.
8.  Keeps the connection open for the duration of the stream.

The controller does not assume that one `recv()` operation corresponds
to one protocol packet.

The descriptor data is accumulated until parsing succeeds.

This is important because TCP is a byte stream rather than a
message-oriented protocol.

------------------------------------------------------------------------

# 6. DeviceDescriptor Parsing

The Android `DeviceDescriptor` implementation was inspected directly in
the VCamdroid source.

The actual serialization order is:

``` text
name
RTSP URL
front resolution count
front resolutions
back resolution count
back resolutions
filter count
filters
```

Each resolution contains:

``` text
uint16 width
uint16 height
```

Each filter contains:

``` text
string name
uint8 category
```

The Linux implementation reproduces this structure and exposes:

``` cpp
struct DeviceDescriptor
{
    std::string name;
    std::string rtspUrl;

    std::vector<Resolution> frontResolutions;
    std::vector<Resolution> backResolutions;

    std::vector<Filter> filters;
};
```

The protocol implementation was based on the Android
deserializer/serializer behavior rather than assuming that the Windows
implementation was authoritative.

------------------------------------------------------------------------

# 7. Activation Packet

A significant M2 protocol finding was the difference between the Windows
serializer and the Android deserializer.

The Windows implementation serializes boolean values as four-byte
integers and omits `focusMode`.

The Android `StreamOptions.deserialize()` implementation instead
expects:

``` text
packet type             1 byte
fps                     int32
width                   int32
height                  int32
camera                  int32
adaptive bitrate        1 byte
bitrate                 int32
minimum bitrate         int32
maximum bitrate         int32
stabilization           1 byte
flash                   1 byte
H265                    1 byte
focus mode              int32
filter count            uint16
filters                 variable
active effect filter    variable
```

All integer fields are big-endian.

The experimentally verified default activation packet is **41 bytes**
and represents:

``` text
Packet type:       0x02 ACTIVATION
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
Filters:           0
Active effect:     ""
```

This packet successfully starts the Android RTSP stream.

------------------------------------------------------------------------

# 8. Dynamic QR Pairing

M2 replaced the manually supplied Linux IP address with runtime
discovery.

The controller enumerates local network interfaces and selects the first
non-loopback IPv4 address.

It then constructs:

``` text
IP_ADDRESS:6969
```

and passes that payload to `qrencode`.

The QR is rendered directly in the terminal using Unicode/ANSI output.

The Android VCamdroid QR scanner successfully reads the generated QR.

### Verified result

A runtime-generated Linux address was displayed as a QR code, Android
scanned it successfully, established the TCP connection, and the
complete video pipeline started.

This removes the need to manually edit an IP address before each
session.

### Current limitation

The address selection is intentionally simple: the first non-loopback
IPv4 interface is used.

A route-aware interface selection strategy may be desirable later on
systems with multiple active interfaces, VPNs, containers, or several
network adapters.

------------------------------------------------------------------------

# 9. V4L2 Loopback Handling

M2 removed the hard-coded `/dev/video2` assumption.

The controller searches:

``` text
/sys/class/video4linux
```

and reads each device's kernel-reported `name`.

It looks for:

``` text
OpenCam
```

and derives the corresponding device path:

``` text
/dev/video*
```

This allows the kernel to assign a different video number without
breaking the application.

For example, the tested system used:

``` text
/dev/video2
```

but the controller does not depend on that number.

------------------------------------------------------------------------

## 9.1 Module Loading

If the OpenCam V4L2 device does not exist, the controller attempts:

``` text
modprobe v4l2loopback \
    devices=1 \
    card_label=OpenCam \
    exclusive_caps=1
```

During final testing after reboot, the module was not automatically
available to the normal user.

A normal-user attempt to load the kernel module failed with:

``` text
Operation not permitted
```

The module could then be loaded manually using the required privileged
command, after which OpenCam discovered the device and streamed
normally.

### Decision

Robust installation, permissions, module loading, systemd/udev
integration, and packaging are intentionally deferred to a later
release/packaging milestone.

M2 therefore treats `v4l2loopback` as a system prerequisite rather than
attempting to hide privileged operations behind the application.

------------------------------------------------------------------------

# 10. FFmpeg Integration

OpenCam launches FFmpeg as a child process.

The current video bridge is equivalent to:

``` text
ffmpeg -hide_banner \
  -rtsp_transport tcp \
  -i <RTSP URL> \
  -an \
  -vf format=yuv420p \
  -f v4l2 \
  -pix_fmt yuv420p \
  -video_size 640x480 \
  <OpenCam V4L2 device>
```

The Linux controller:

-   forks FFmpeg,
-   starts it with `execlp`,
-   records its PID,
-   keeps the control connection alive,
-   sends `SIGTERM` when the Android client disconnects,
-   waits for FFmpeg to exit.

The use of `-an` intentionally keeps the current M2 bridge video-only.

Audio is not considered implemented by M2.

------------------------------------------------------------------------

# 11. RTSP Readiness Detection

The controller does not immediately launch the FFmpeg bridge after
sending activation.

Instead, it repeatedly probes the Android RTSP endpoint using `ffprobe`.

The current implementation performs up to:

``` text
10 attempts
```

with approximately:

``` text
500 ms
```

between attempts.

During testing, the first several probes failed while the Android RTSP
server initialized, followed by a successful probe.

Once the RTSP endpoint became available, OpenCam launched FFmpeg.

This avoids racing FFmpeg against Android's stream initialization.

------------------------------------------------------------------------

# 12. Reconnection Handling

A major M2 improvement was moving from a one-shot controller to a
reusable session loop.

The lifecycle is now:

``` text
Start OpenCam
      │
      ▼
Display QR
      │
      ▼
Wait for Android
      │
      ▼
Receive descriptor
      │
      ▼
Activate stream
      │
      ▼
Start FFmpeg
      │
      ▼
Video bridge active
      │
      ▼
Android disconnects
      │
      ▼
Stop FFmpeg
      │
      ▼
Return to listener
      │
      ▼
Wait for Android again
```

The Linux process itself does not need to be restarted.

This was experimentally verified by disconnecting Android, allowing
FFmpeg cleanup, scanning the same QR again, and establishing a new
stream.

------------------------------------------------------------------------

# 13. Final M2 Test Results

## 13.1 Pairing

**Result: PASS**

-   Linux determines its current IPv4 address.
-   OpenCam generates a QR payload dynamically.
-   QR is displayed in the terminal.
-   Android scans it.
-   Android connects to TCP port 6969.

------------------------------------------------------------------------

## 13.2 Device Detection

**Result: PASS**

The Linux controller receives and parses the Android `DeviceDescriptor`.

The controller displays information including:

``` text
Device name
RTSP URL
Front resolution count
Back resolution count
Filter count
```

------------------------------------------------------------------------

## 13.3 Activation

**Result: PASS**

The Linux-generated 41-byte activation packet is accepted by Android.

Android starts its RTSP server.

------------------------------------------------------------------------

## 13.4 RTSP

**Result: PASS**

The Linux controller detects the RTSP endpoint after startup delay.

The stream can then be consumed by FFmpeg.

------------------------------------------------------------------------

## 13.5 V4L2

**Result: PASS**

The OpenCam virtual camera is discovered dynamically.

The final tested device was:

``` text
/dev/video2
```

but no hard-coded device number is required.

------------------------------------------------------------------------

## 13.6 OBS

**Result: PASS**

OBS successfully received the Android camera through the OpenCam V4L2
device.

------------------------------------------------------------------------

## 13.7 Browser Webcam Capture

**Result: PASS**

The OpenCam virtual camera was also recognized by a browser-based webcam
application.

This verifies that the output is not limited to OBS.

------------------------------------------------------------------------

## 13.8 Reconnection

**Result: PASS**

Android can disconnect and reconnect without restarting the Linux
controller.

The previous FFmpeg process is cleaned up before the new session begins.

------------------------------------------------------------------------

## 13.9 End-to-End Latency

Observed end-to-end latency was approximately:

``` text
~2–4 seconds
```

with roughly:

``` text
~3 seconds
```

being typical during testing.

The video was observed to be smooth.

Latency optimization was intentionally deferred.

------------------------------------------------------------------------

# 14. Current Video Configuration

The primary M2 test configuration is:

``` text
Resolution:      640 × 480
Frame rate:      30 FPS
Codec:           H.264
Pixel format:    YUV420P
Transport:       RTSP over TCP
Output:          V4L2 loopback
```

This configuration is a **validation configuration**, not a final
OpenCam resolution target.

The purpose of using 640×480 during M1/M2 was to validate the complete
control and media pipeline before introducing higher-resolution
performance variables.

------------------------------------------------------------------------

# 15. What M2 Proved

M2 established that Linux can replace the original Windows controller
for the core VCamdroid workflow.

The complete chain is now:

``` text
Android Camera
      ↓
VCamdroid RTSP
      ↓
Wi-Fi
      ↓
OpenCam Linux Controller
      ↓
FFmpeg
      ↓
V4L2 Loopback
      ↓
/dev/video*
      ↓
OBS / Browser / Webcam Applications
```

The control path is:

``` text
Android
   │
   │ TCP :6969
   ▼
OpenCam Linux Controller
```

The two paths work together to provide a reusable Linux camera bridge.

------------------------------------------------------------------------

# 16. Known Limitations

M2 is intentionally not the final OpenCam architecture.

The following limitations remain:

### 16.1 No OpenCam authentication

The TCP control connection currently does not authenticate the Android
device.

QR pairing currently provides discovery rather than cryptographic
authentication.

### 16.2 No encrypted OpenCam media/control layer

The current implementation should not be treated as secure against a
hostile or untrusted LAN.

Security hardening is deferred.

### 16.3 640×480 is currently hard-coded in the FFmpeg output path

The Android descriptor already exposes camera resolutions, but M2 does
not yet implement complete resolution negotiation.

### 16.4 Audio is not implemented

The current FFmpeg bridge explicitly disables audio.

Microphone capture and A/V synchronization remain future work.

### 16.5 Latency remains relatively high

Approximately 2--4 seconds was observed.

No serious latency optimization has yet been attempted.

### 16.6 V4L2 loopback installation is not packaged

The kernel module currently requires system-level setup.

### 16.7 Network interface selection is basic

The first non-loopback IPv4 address is selected.

Multi-interface environments may require better route-aware selection.

### 16.8 FFmpeg/ffprobe command execution is basic

The current RTSP readiness probe uses a shell command through
`std::system()`.

This is acceptable for the current controlled prototype but should be
hardened before release.

------------------------------------------------------------------------

# 17. Engineering Decisions Made in M2

Several decisions were deliberately made to keep M2 small and
understandable.

### Native C++ controller

The temporary Python implementation served as a protocol and pipeline
experiment.

The permanent Linux implementation is C++17.

### CMake

CMake provides a simple, conventional build system without introducing
unnecessary project infrastructure.

### Dynamic device discovery

The application does not assume `/dev/video2`.

### Runtime QR generation

The QR code is generated from the current Linux network address instead
of storing a fixed IP.

### Runtime FFmpeg process management

FFmpeg is treated as a child process owned by the current OpenCam
session.

### Graceful disconnect handling

Android disconnecting is treated as a session event rather than an
application-fatal error.

### System prerequisite instead of hidden privilege escalation

OpenCam does not attempt to run `sudo` internally.

The current implementation may attempt normal `modprobe`; proper
privileged installation/setup is deferred.

------------------------------------------------------------------------

# 18. Verification Classification

To keep the technical record honest, M2 results can be classified as
follows.

## Experimentally verified

-   Dynamic QR pairing.
-   Android TCP connection to Linux.
-   DeviceDescriptor reception and parsing.
-   41-byte activation packet.
-   Android RTSP startup.
-   FFmpeg RTSP reception.
-   V4L2 loopback output.
-   Dynamic `/dev/video*` discovery.
-   OBS webcam capture.
-   Browser webcam capture.
-   Android disconnect/reconnect.
-   FFmpeg cleanup.
-   Reusing the same OpenCam process for another Android session.
-   Smooth 640×480 video.
-   Approximately 2--4 seconds end-to-end latency during testing.

## Repository-derived

-   Android TCP connection behavior.
-   DeviceDescriptor serialization structure.
-   Packet type values.
-   StreamOptions deserialization layout.
-   RTSP endpoint structure.
-   Android activation behavior.
-   Windows/Linux protocol differences identified during source
    inspection.

## Deferred / not yet verified

-   720p/1080p/1440p/4K performance.
-   Hardware/codec behavior across different Android devices.
-   Low-latency optimization.
-   USB/ADB transport.
-   Microphone/audio transport.
-   A/V synchronization.
-   OpenCam authentication.
-   End-to-end encryption.
-   Robust multi-interface networking.
-   Production packaging.
-   Automatic v4l2loopback installation.
-   Windows implementation.

------------------------------------------------------------------------

# 19. M2 Conclusion

Milestone 2 is complete.

The original M1 proof-of-concept pipeline has been converted into a
reusable Linux-native controller with:

``` text
✓ C++ implementation
✓ CMake build
✓ Dynamic QR pairing
✓ TCP control handling
✓ DeviceDescriptor parsing
✓ Correct Android activation packet
✓ Dynamic V4L2 discovery
✓ FFmpeg process management
✓ RTSP readiness detection
✓ Reconnection support
✓ OBS compatibility
✓ Browser webcam compatibility
✓ Clean session lifecycle
```

The most important result is architectural:

> **Linux can now act as the OpenCam host/controller without requiring
> the original Windows controller.**

The current implementation should be considered a **functional Linux MVP
foundation**, not yet a production-secure or performance-optimized
release.

------------------------------------------------------------------------

# 20. Recommended Post-M2 Roadmap

Based on the findings of M1 and M2, the next development stages should
focus on:

``` text
M3 — Pairing & Network Security
      │
      ├── authenticated pairing
      ├── session identity
      ├── LAN threat model
      └── direct/hotspot networking model
      │
      ▼
M4 — Resolution, Performance & Latency
      │
      ├── 720p
      ├── 1080p
      ├── higher-resolution capability testing
      ├── buffering analysis
      └── latency reduction
      │
      ▼
M5 — USB / ADB Transport
      │
      ├── ADB forwarding
      ├── transport abstraction
      └── low-latency mode
      │
      ▼
M6 — Audio + A/V Synchronization
      │
      ├── microphone capture
      ├── audio transport
      ├── PipeWire/ALSA output
      └── timestamp synchronization
      │
      ▼
M7 — Android UI + OpenCam Rebrand
      │
      ├── OpenCam branding
      ├── connection modes
      ├── resolution/FPS controls
      └── user-facing status/errors
      │
      ▼
M8 — Windows Implementation
      │
      ▼
M9 — Packaging / Release / Final Testing
```

This roadmap is intentionally based on the technical gaps discovered
during M2 rather than assuming that the original milestone ordering
still applies.

------------------------------------------------------------------------

## Final M2 State

``` text
                OPEN CAM M2
                    │
          ┌─────────┴─────────┐
          │                   │
       CONTROL              VIDEO
          │                   │
       TCP 6969           RTSP 8554
          │                   │
          └─────────┬─────────┘
                    │
              Linux Controller
                    │
                 FFmpeg
                    │
               V4L2 loopback
                    │
          ┌─────────┴─────────┐
          │                   │
         OBS               Browser
```

**Milestone 2: COMPLETE and ready to freeze.**
