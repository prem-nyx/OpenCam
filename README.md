<div align="center">

# OpenCam

### Turn your Android phone into a wireless camera for your computer.

[![Development Status](https://img.shields.io/badge/status-active%20development-orange.svg)](#-current-status)
[![GitHub Stars](https://img.shields.io/github/stars/prem-nyx/OpenCam?style=flat&logo=github)](https://github.com/prem-nyx/OpenCam/stargazers)

</div>

---

## 📷 What is OpenCam?

OpenCam is an open-source project for turning Android devices into
wireless camera and, eventually, microphone sources for computers.

The project started from the existing **VCamdroid** codebase and is
being developed toward a broader cross-platform architecture.

The current development focus is **Linux**, with native Linux control,
V4L2 integration, secure device pairing, and a foundation for future
cross-platform support.

---

## 🚧 Current Status

OpenCam is under active development.

| Milestone | Status |
|---|---|
| M1 — Android → Linux video pipeline | ✅ Complete |
| M2 — Linux controller | ✅ Complete |
| M3 — Pairing & network security | 🚧 Next |
| M4 — Resolution & performance | 📋 Planned |
| M5 — USB / ADB transport | 📋 Planned |
| M6 — Audio & A/V synchronization | 📋 Planned |
| M7 — Android UI & OpenCam rebrand | 📋 Planned |
| M8 — Windows implementation | 📋 Planned |
| M9 — Packaging & release | 📋 Planned |

---

## 🧩 How It Works

The current Linux implementation uses the following pipeline:

```text
┌──────────────────┐
│   Android Phone  │
│     VCamdroid    │
└────────┬─────────┘
         │
         │ RTSP
         ▼
┌──────────────────┐
│ Linux Controller │
│     OpenCam      │
└────────┬─────────┘
         │
         │ FFmpeg
         ▼
┌──────────────────┐
│  V4L2 Loopback   │
│   /dev/videoX    │
└────────┬─────────┘
         │
         ▼
┌──────────────────┐
│ OBS / Browser /  │
│ Other Applications│
└──────────────────┘
```

The Android device is controlled through a TCP connection while the
camera stream is delivered through RTSP.

---

## 🐧 Current Linux Support

The Linux controller currently provides:

- 📱 Android device pairing through QR codes
- 🔌 TCP-based device control
- 📡 RTSP stream activation
- 🎥 FFmpeg-based video bridging
- 📹 V4L2 loopback camera output
- 🔄 Device reconnect support
- 🌐 Dynamic local-network address detection
- 🖥️ Compatibility with applications such as OBS and browser-based
  webcam capture

The current validated configuration uses a 640×480 H.264 video stream.
Higher resolutions and performance improvements are planned for later
milestones.

---

## 🗺️ Roadmap

OpenCam is being developed incrementally rather than attempting to
replace the original implementation all at once.

### M1 — Android → Linux Video Pipeline

- Android camera streaming
- RTSP reception
- FFmpeg integration
- V4L2 loopback
- OBS/browser compatibility

**Status: ✅ Complete**

### M2 — OpenCam Linux Controller

- Native Linux controller
- VCamdroid protocol implementation
- Device descriptor parsing
- Activation packet generation
- Dynamic QR pairing
- Virtual camera discovery
- Reconnection handling

**Status: ✅ Complete**

### M3 — Pairing & Network Security

- Device identity
- Authenticated pairing
- Session authentication
- Replay protection
- LAN security
- Direct/hotspot networking investigation

**Status: 🚧 Next**

### M4 — Resolution & Performance

- Capability-driven resolution selection
- 720p / 1080p investigation
- Latency reduction
- Buffering improvements
- CPU/GPU performance investigation

**Status: 📋 Planned**

### M5 — USB / ADB Transport

- USB connectivity
- ADB transport
- Lower-latency transport investigation
- Wireless vs wired transport selection

**Status: 📋 Planned**

### M6 — Audio & A/V Synchronization

- Microphone streaming
- Audio transport
- Video/audio synchronization
- Wireless microphone functionality

**Status: 📋 Planned**

### M7 — Android UI & OpenCam Rebrand

- OpenCam Android interface
- Removal of legacy VCamdroid-facing UI
- OpenCam visual identity
- Application/package rework

**Status: 📋 Planned**

### M8 — Windows Implementation

- Native Windows controller
- Windows virtual-camera integration
- Cross-platform controller architecture

**Status: 📋 Planned**

### M9 — Packaging & Release

- Installation workflow
- Dependency management
- v4l2loopback setup
- Documentation
- Release builds
- Final testing

**Status: 📋 Planned**

---

## 🌱 Project Origin

OpenCam builds upon the open-source
[VCamdroid](https://github.com/darusc/VCamdroid) project by **Darusc**.

VCamdroid provided the original Android camera streaming architecture
and Windows-side implementation that OpenCam is building upon.

OpenCam is being developed in a different direction, with a focus on:

- Native Linux support
- Linux-side device control
- V4L2 virtual-camera integration
- Cross-platform architecture
- Secure device pairing
- Multiple transport options
- Higher-resolution streaming
- Performance and latency improvements
- Future audio and A/V synchronization
- A dedicated OpenCam Android interface

The original upstream project and its contributors will be properly
attributed as part of OpenCam's licensing and attribution work.

---

## 🙏 Acknowledgements

OpenCam would not exist without the projects and contributors whose
work provided its foundation.

### VCamdroid

Original project:

https://github.com/darusc/VCamdroid

Created by **Darusc**.

OpenCam contains and builds upon code originating from VCamdroid.
The applicable upstream copyright notices and license terms will be
preserved.

### Softcam

OpenCam also builds upon components originating from **Softcam**.

The applicable upstream attribution and license terms will be
documented as part of the project's licensing work.

### Other Dependencies

OpenCam uses additional open-source libraries and system components.
Their licenses and attribution notices will be documented as the
project approaches its release stage.

---

## 📜 License

OpenCam's licensing and third-party attribution are currently being
formalized.

The project contains code originating from existing open-source
projects, including VCamdroid and Softcam. Their respective copyright
notices and license requirements will be preserved.

A complete attribution and licensing document will be included before
the first formal OpenCam release.

---

<div align="center">

### Built by V3NOM

[GitHub](https://github.com/prem-nyx)

</div>
