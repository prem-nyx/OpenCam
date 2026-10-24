# OpenCam

**Use an Android phone as a camera for a Linux computer.**

OpenCam is an open-source project that streams video from an Android
device to a Linux computer over a local network. The Linux controller
receives the stream and exposes it as a V4L2 virtual camera, allowing
compatible applications such as OBS to use the phone as a camera source.

> **Project status:** Active development. The validated target is
> Android and Linux over Wi-Fi. OpenCam is not yet a production-ready
> secure communications system.

## Features

-   QR-based pairing between the Android client and Linux controller
-   Mutually authenticated control sessions
-   AES-256-GCM encryption for control messages
-   HKDF-SHA256 session-key derivation with separate directional keys
-   Sequence validation to reject replayed, duplicated, or out-of-order
    control frames
-   RTSP video transport from Android to Linux
-   In-process FFmpeg libraries for RTSP probing, decoding, and pixel
    conversion
-   V4L2 loopback output for Linux camera applications
-   RTSP endpoint validation and bounded control-protocol parsing

## How it works

``` text
Android phone
  ├── Authenticated, encrypted control channel ──> OpenCam Linux controller
  └── RTSP video stream ─────────────────────────> OpenCam Linux controller
                                                      │
                                                      ▼
                                             FFmpeg libraries
                                                      │
                                                      ▼
                                                V4L2 loopback
                                                      │
                                                      ▼
                                               OBS / other apps
```

The control and media paths are separate. The control channel is
authenticated and encrypted. The RTSP media path currently uses
`rtsp://` and is **not encrypted with TLS**.

## Current implementation

  Component              Current implementation
  ---------------------- --------------------------------------
  Client                 Android
  Controller             Linux
  Network                Wi-Fi / TCP
  Pairing                QR-delivered, random pairing secret
  Control protocol       OCAM v2
  Control encryption     AES-256-GCM
  Key derivation         HKDF-SHA256
  Media transport        RTSP over TCP
  Video output           V4L2 loopback
  Video codec tested     H.264
  Resolution validated   640 × 480
  Consumers              OBS and compatible V4L2 applications

Resolution, latency, and compatibility can vary with the device,
network, codec configuration, and host system.

## Security

OpenCam's current security work includes mutual authentication,
encrypted control frames, protocol bounds and validation, RTSP endpoint
validation, and removal of the FFmpeg subprocess-argument path for media
URLs.

The Android RTSP server fork also includes RTSP Basic-authentication
hardening and unit tests. Basic authentication is not encryption, and it
does not protect media confidentiality on its own.

**Known limitations** - RTSP media is not yet protected by RTSPS/TLS. -
The control protocol does not currently provide forward secrecy. - The
media URL and derived credential remain present in process memory while
streaming. - Additional resource-exhaustion, malformed-peer,
multi-interface, and lifecycle testing remains. - Windows and non-Wi-Fi
transports are outside the current validated scope.

See [`SECURITY.md`](SECURITY.md) for the security architecture,
validation notes, limitations, and planned work.

## Development status

  -----------------------------------------------------------------------
  Milestone                           Status
  ----------------------------------- -----------------------------------
  M1 --- Android-to-Linux video       Complete
  pipeline                            

  M2 --- Linux controller and pairing Complete
  flow                                

  M3 --- Pairing and control-channel  Substantially complete; media TLS
  security                            remains open

  M4 --- Resolution and performance   Planned

  M5 --- USB / ADB transport          Planned

  M6 --- Microphone and A/V           Planned
  synchronization                     

  M7 --- Android UI and rebrand       In progress

  M8 --- Windows implementation       Planned

  M9 --- Packaging and release        Planned
  
  -----------------------------------------------------------------------

## Development and testing

For the Android RTSP server fork, the authentication unit tests can be
run from the fork repository with:

``` bash
./gradlew :rtspserver:testDebugUnitTest
```

The Linux controller uses CMake and the system's FFmpeg development
libraries. Consult the build files and project-specific setup notes for
required packages and configuration.

## Related repositories

-   OpenCam: <https://github.com/prem-nyx/OpenCam>
-   OpenCam RTSP server fork: <https://github.com/prem-nyx/RTSP-Server>
-   Upstream RTSP server project:
    <https://github.com/pedroSG94/RTSP-Server>
-   VCamdroid project that provided part of the project's foundation:
    <https://github.com/darusc/VCamdroid>

The RTSP fork maintains OpenCam-specific changes separately from the
upstream project.

## Attribution and licensing

OpenCam builds on existing open-source software, including work
originating from VCamdroid and RTSP-Server, along with other libraries
and system components. Preserve applicable upstream copyright notices
and license terms. A complete dependency and attribution review is still
required before a formal release.

## Contributing

OpenCam is under active development. Changes to security-sensitive code
should include tests and should not claim stronger guarantees than the
implementation demonstrates. Do not switch media endpoints to `rtsps://`
until server-side TLS, certificate provisioning, peer verification, and
end-to-end streaming have been implemented and tested.

## Maintainer

Developed by [V3NOM](https://github.com/prem-nyx).
