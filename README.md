<div align="center">

# OpenCam

### Turn your Android phone into a camera for your computer.

[![Development Status](https://img.shields.io/badge/status-active%20development-orange.svg)](#-current-status)
[![GitHub Stars](https://img.shields.io/github/stars/prem-nyx/OpenCam?style=flat&logo=github)](https://github.com/prem-nyx/OpenCam/stargazers)

</div>

---

## 📷 What is OpenCam?

OpenCam is an open-source project that turns Android devices into cameras
for computers over a local network.

The current implementation focuses on **Linux + Android**, providing:

- QR-based device pairing
- Authenticated control communication
- Encrypted control frames
- RTSP video streaming
- FFmpeg-based video bridging
- V4L2 virtual-camera output

Future development will extend OpenCam toward microphone streaming,
A/V synchronization, additional transports, and broader platform support.

---

## 🚧 Current Status

OpenCam is under active development.

| Milestone | Status |
|---|---|
| M1 — Android → Linux video pipeline | ✅ Complete |
| M2 — Linux controller | ✅ Complete |
| M3 — Pairing & network security | 🟢 Substantially complete |
| M4 — Resolution & performance | 📋 Planned |
| M5 — USB / ADB transport | 📋 Planned |
| M6 — Audio & A/V synchronization | 📋 Planned |
| M7 — Android UI & OpenCam rebrand | 🚧 In progress |
| M8 — Windows implementation | 📋 Planned |
| M9 — Packaging & release | 📋 Planned |

> **Current validated scope:** Linux ↔ Android over Wi-Fi.
> Other platforms and transports remain future work.

---

## 🧩 Current Implementation

| Component | Current implementation |
|---|---|
| Platform | Linux + Android |
| Network transport | Wi-Fi / TCP |
| Device pairing | QR-based |
| Control authentication | Mutual authentication |
| Control encryption | AES-256-GCM |
| Key derivation | HKDF-SHA256 |
| Media transport | RTSP over TCP |
| Video codec | H.264 |
| Linux output | V4L2 virtual camera |
| Validated resolution | 640×480 |
| Camera consumers | OBS, browsers, and other V4L2 applications |

The control and media channels are separate.

The current control plane is authenticated and encrypted. The RTSP media
plane does not yet provide TLS/RTSPS encryption.

For the detailed security architecture, validation results, and known
limitations, see [`SECURITY.md`](SECURITY.md).

---

## 🗺️ How It Works

The current Linux implementation uses the following pipeline:

```text
┌──────────────────┐
│   Android Phone  │
│     OpenCam      │
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
┌───────────────────┐
│ OBS / Browser /   │
│ Other Applications│
└───────────────────┘
```

The Android device is paired and controlled through a TCP connection.
The camera stream is delivered through RTSP, received by the Linux
controller, and bridged into a V4L2 virtual camera using FFmpeg.

---

## 🔐 Security

Security hardening is a major part of the current development milestone.

The Linux ↔ Android control plane currently provides:

- 🔑 QR-based pairing using a random pairing secret
- 🤝 Mutual authentication
- 🔒 AES-256-GCM encrypted control frames
- 🧬 HKDF-SHA256 session key derivation
- ↔️ Directional session keys
- 🔢 Sequence-based replay/reordering protection
- 📦 Bounded protocol parsing
- 🌐 RTSP endpoint validation
- 🛡️ Shell-free FFprobe/FFmpeg process execution
- 🧰 FFmpeg process and file-descriptor isolation

The implementation has been validated with a real Android device over
Wi-Fi, including encrypted control traffic and end-to-end video delivery.

### Current security limitations

The security model is still under development.

In particular:

- RTSP media is not currently protected by TLS/RTSPS.
- The current FFmpeg media pipeline can expose the RTSP credential through
  the process command line.
- The current protocol does not provide forward secrecy.
- Additional resource, malformed-peer, and network edge-case testing
  remains ongoing.

See [`SECURITY.md`](SECURITY.md) for the complete security status and
future hardening work.

---

## 🛠️ Development Roadmap

### M1 — Android → Linux Video Pipeline

- Android camera streaming
- RTSP reception
- FFmpeg integration
- V4L2 loopback
- OBS/browser compatibility

**Status: ✅ Complete**

### M2 — OpenCam Linux Controller

- Native Linux controller
- Device descriptor parsing
- Activation packet generation
- Dynamic QR pairing
- Virtual-camera integration
- Connection handling

**Status: ✅ Complete**

### M3 — Pairing & Network Security

- QR-based device pairing
- Mutual authentication
- Encrypted control sessions
- Replay/reordering protection
- RTSP endpoint validation
- Shell-injection remediation
- FFmpeg process isolation
- Real-device Wi-Fi validation
- Security audit and remediation

**Status: 🟢 Substantially complete**

Remaining work includes media-plane encryption, additional network
edge-case testing, and further resource/error-path hardening.

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
- Removal of legacy VCamdroid-facing UI references
- OpenCam visual identity
- Android UI refinement
- Application/package cleanup

**Status: 🚧 In progress**

> The Android application is now branded as **OpenCam**. The existing
> Android package identifier remains unchanged for the current development
> phase.

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
