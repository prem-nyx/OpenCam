# OpenCam — Milestone 3 Progress Report

**Date:** 15 October 2026  
**Milestone:** M3 — Pairing, Network Security & Security Hardening  
**Scope:** Linux controller ↔ Android client over Wi-Fi

---

## 1. Executive Summary

Milestone 3 focused on moving OpenCam from a functional network prototype
toward a hardened Linux ↔ Android Wi-Fi implementation.

The work covered:

- security auditing of the existing control and media architecture
- removal of shell-mediated RTSP probing
- bounded network protocol parsing
- QR-based pairing secrets
- mutual authentication
- authenticated session establishment
- AES-256-GCM encrypted control frames
- HKDF-SHA256 key derivation
- directional session keys
- strict frame sequencing and replay/reordering protection
- RTSP endpoint validation
- in-process FFmpeg library media pipeline
- removal of FFmpeg/FFprobe subprocess command-line credential exposure
- bounded protocol handling and generic media error reporting
- Android protocol and validation hardening
- real-device Wi-Fi integration
- end-to-end RTSP → FFmpeg → V4L2 validation
- security-focused automated testing
- runtime packet-capture verification of encrypted control traffic

The current M3 implementation is **substantially complete for the
Linux ↔ Android Wi-Fi control-plane scope**.

The major remaining media security limitation is that the RTSP media plane
is not currently protected by TLS/RTSPS. The previously identified exposure
of the authenticated RTSP URL through FFmpeg subprocess arguments has been
remediated by replacing the probe and media subprocesses with in-process
FFmpeg library calls. Residual in-memory credential exposure remains outside
the scope of that specific fix.

---

# 2. M3 Objectives

The milestone was intended to address the main security weaknesses
identified in the earlier OpenCam network implementation.

### Primary objectives

1. Authenticate Android ↔ Linux peers.
2. Protect the control channel against tampering and replay.
3. Encrypt control-plane traffic.
4. Validate peer-supplied RTSP endpoints.
5. Remove shell-injection risks.
6. Bound network input and connection resources.
7. Harden Android network parsing and lifecycle handling.
8. Isolate FFmpeg from OpenCam control sockets and descriptors.
9. Improve diagnostics without exposing secrets unnecessarily.
10. Validate the complete implementation on a real Android device.
11. Document remaining security limitations.

---

# 3. Initial Security Findings

The M3 security investigation identified several weaknesses in the
original architecture.

| Finding | Description | Current status |
|---|---|---|
| M3-01 | RTSP URL reached `std::system()` | 🟢 Fixed |
| M3-02 | Unauthenticated peer-controlled media activation | 🟢/🟡 Hardened |
| M3-03 | Descriptor/resource accumulation | 🟡 Partially hardened |
| M3-04 | Android TCP framing/parser weaknesses | 🟢 Hardened |
| M3-05 | Windows unsafe deserialization | 📋 Outside current Linux scope |
| M3-06 | Wildcard listener/address selection | 🟢 Hardened |
| M3-07 | RTSP authentication/TLS limitations | 🟡 Partial |
| M3-08 | Android lifecycle/timeouts/EOF handling | 🟢 Hardened |
| M3-09 | Logging of sensitive connection information | 🟢 Improved |
| M3-10 | Cross-platform activation mismatch | 🟡 Outside current Linux scope |
| M3-11 | FFmpeg inherited control/listener FDs | 🟢 Fixed |

The current milestone intentionally focuses on **Linux ↔ Android Wi-Fi**.
Windows Wi-Fi support is treated as future work rather than being claimed
as part of the current implementation.

---

# 4. Control Protocol v2

The control protocol was upgraded from the earlier authenticated
plaintext design to **protocol version 2**.

The protocol identifier remains:

```text
OCAM
```

The current version is:

```text
OCAM v2
```

The frame header contains:

```text
magic
version
message type
payload length
```

The maximum payload size remains bounded at:

```text
64 KiB
```

The protocol uses explicit message types for:

- `SERVER_HELLO`
- `CLIENT_AUTH`
- `SERVER_AUTH`
- `DESCRIPTOR`
- `ACTIVATION`
- `ERROR_REPORT`

---

# 5. QR Pairing and Secret Handling

The QR code now carries a randomly generated pairing secret.

The secret is:

- 256 bits
- intended for a single successful pairing
- rotated after successful pairing
- time-limited

The pairing secret is not intentionally written to normal OpenCam logs.

The current pairing format is based on the OpenCam protocol v2 structure
and includes the Linux peer address, control port, and 64-character
hexadecimal secret.

---

# 6. Mutual Authentication

The Linux controller and Android client perform mutual authentication
before establishing the authenticated control session.

The authentication proofs use HMAC-SHA256 with versioned domains:

```text
OpenCam client proof v2
OpenCam server proof v2
```

Both sides must demonstrate knowledge of the pairing secret.

An unauthenticated network peer therefore cannot proceed to the
authenticated descriptor/activation phase.

---

# 7. AES-256-GCM Control-Channel Encryption

The largest security improvement completed during M3 was migration from
authenticated-but-readable control frames to **AES-256-GCM encrypted
control frames**.

After the authentication handshake, the following control messages are
encrypted:

- device descriptors
- activation packets
- error reports

The outer protocol header remains visible because it is required for
framing and routing.

The encrypted payload contains:

```text
sequence || ciphertext || GCM authentication tag
```

The sequence number and outer header are incorporated into the
authenticated encryption construction.

Therefore, a packet observer can still identify the OpenCam frame
structure, but cannot read the encrypted application payload.

---

# 8. Session Key Derivation

Session keys are derived using **HKDF-SHA256**.

The current session derivation uses:

```text
salt = serverNonce || clientNonce
IKM  = pairing secret
info = "OpenCam control v2 session"
```

The resulting session material is then used to derive independent
directional keys.

The directional derivation uses:

```text
OpenCam control v2 server-to-client
OpenCam control v2 client-to-server
```

This separates encryption keys for the two communication directions.

The implementation uses platform cryptographic primitives rather than
a custom AES implementation.

---

# 9. Nonce and Sequence Construction

AES-GCM nonces combine a direction-specific prefix with the frame
sequence number.

Current direction identifiers are:

```text
SRVR
CLNT
```

followed by the 64-bit sequence value.

Receivers require the exact expected sequence number.

This prevents:

- replay of previously accepted frames
- duplicate frames
- reordered frames
- simple sequence manipulation

AES-GCM additionally detects ciphertext modification.

---

# 10. Android Protocol Hardening

The Android client was updated to support the v2 encrypted protocol.

The Android implementation now includes:

- v2 protocol negotiation
- bounded frame parsing
- exact byte-count reads
- connection timeouts
- EOF handling
- explicit authentication ordering
- separate send/receive locking
- AES-GCM encryption/decryption
- HKDF-based key derivation
- directional session keys
- sequence validation
- tamper detection
- stronger descriptor and activation validation

A previous full-duplex synchronization issue involving the authenticated
channel was also corrected by separating send and receive synchronization.

---

# 11. Linux RTSP Endpoint Validation

The Linux controller validates the RTSP endpoint supplied by the
authenticated Android peer before using it.

The current accepted structure is restricted to the authenticated peer's
expected IPv4 address and RTSP port.

The expected endpoint uses:

```text
rtsp://<peer>:8554/live/<random-path>
```

The endpoint validator rejects unsupported or ambiguous forms including:

- unexpected schemes
- unexpected hosts
- unexpected ports
- userinfo
- query strings
- fragments
- alternate endpoint structures

The stream path is randomized per streaming session.

---

# 12. Media Pipeline and Process-Execution Remediation

The earlier RTSP probe used shell-mediated execution, allowing peer-controlled
RTSP URL data to reach a shell command. The probe was first changed away from
shell command construction, and the latest media-pipeline migration removes the
FFmpeg/FFprobe subprocesses from the active RTSP path entirely.

The current implementation uses FFmpeg libraries in process:

- `libavformat` opens and probes the RTSP stream.
- `libavcodec` decodes video frames.
- `libswscale` converts frames to the V4L2-compatible YUV420 format.
- A dedicated media worker writes frames to the V4L2 loopback device.

The authenticated RTSP URL is held in process memory and is not passed as an
FFmpeg/FFprobe command-line argument. This removes the previously identified
credential exposure through `/proc/<ffmpeg-pid>/cmdline` for the active media
pipeline.

Unrelated process-execution paths remain: the QR display code launches
`qrencode` with fixed arguments and supplies the payload through stdin, while
the V4L2 loopback loader runs a fixed `modprobe` command. Neither receives the
authenticated RTSP URL; they remain separate process-hardening considerations.

---

# 13. In-Process Media Pipeline and Lifecycle

The Linux controller now performs RTSP probing and media decoding through
in-process FFmpeg libraries rather than a separate FFmpeg child process.

The media worker:

- opens the authenticated RTSP endpoint using FFmpeg libraries
- locates and decodes the video stream
- converts frames to YUV420 at the pipeline's configured output size
- writes frames to the V4L2 loopback device
- runs separately from the control loop so control messages can continue to
  be handled during streaming
- uses a stop/interrupt path so shutdown can terminate media work

The developer verified the new executable with a real Android stream. The
stream ran for several minutes without reported issues, restarting the stream
was smooth, and the disconnect/shutdown check was reported clean.

The previous process tree containing a separate FFmpeg media child no longer
applies to this architecture. A process inspection during the current
verification showed the OpenCam executable without a separate FFmpeg or
FFprobe process.

---

---

# 14. Media Diagnostics and Error Handling

The in-process pipeline suppresses FFmpeg library log output to avoid
accidentally printing authenticated URL contents. Media failures are surfaced
through generic application-level errors rather than echoing peer-provided
error text.

The project also includes diagnostic-redaction code and an automated test
target for it. These measures reduce accidental disclosure through normal
diagnostics; they do not prevent exposure through process memory, debugging,
crash dumps, or other host-level access.

---

# 15. RTSP Credential Exposure Remediation

The previous FFmpeg CLI pipeline included the authenticated RTSP URL in the
child process argument list. That made the credential inspectable through
`/proc/<ffmpeg-pid>/cmdline`; FFmpeg diagnostics could also reproduce the URL.

The current in-process media pipeline removes that specific exposure path:

- RTSP probing uses `libavformat` in process.
- Video processing uses `libavformat`, `libavcodec`, and `libswscale` in process.
- No FFmpeg or FFprobe subprocess is used for the active media pipeline.
- The authenticated URL is not passed in a media-process argument list.
- FFmpeg library log output is suppressed.

### Verification performed

- A clean Linux build succeeded.
- All three registered Linux CTest tests passed on rerun:
  `opencam_security_tests`, `opencam_shell_injection_regression`, and
  `opencam_diagnostic_redaction_tests`.
- The real Android-to-Linux stream ran for several minutes without reported
  issues.
- Stream restart was reported smooth.
- The developer reported disconnect/shutdown checks were clean.
- Process inspection showed OpenCam without a separate FFmpeg/FFprobe process.

### Scope of the fix

This remediates the specific **credential exposure through FFmpeg/FFprobe
subprocess arguments**. The URL and derived credential still exist in OpenCam's
memory while streaming, and this change does not provide TLS/RTSPS encryption
for the RTSP media plane. It should not be represented as complete media
confidentiality or a full security audit.

---

---

# 16. Real-Device Wi-Fi Integration

The updated implementation was tested using an actual Android device
and Linux host over Wi-Fi.

The verified sequence for the earlier subprocess-based implementation was:

```text
Wi-Fi mode selected
        ↓
QR pairing
        ↓
TCP control connection
        ↓
Mutual authentication
        ↓
Encrypted control session
        ↓
Descriptor received
        ↓
Activation sent
        ↓
Android RTSP server starts
        ↓
Linux RTSP readiness probe
        ↓
FFmpeg starts
        ↓
V4L2 virtual camera receives video
```

The implementation was tested while a USB cable was physically attached
to the Android device to verify that Wi-Fi mode did not silently fall
back to USB/loopback behavior.

---

# 17. Real-Device Encryption Verification

The encrypted control protocol was also verified at the network-packet
level.

A packet capture was taken against the control connection:

```text
tcpdump -i any -nn -X -s 0 'tcp port 6969'
```

A captured frame contained the visible protocol header:

```text
OCAM
02
05
00 00 00 40
```

The observed fields corresponded to:

```text
magic      = OCAM
version    = 2
type       = ACTIVATION
wire length = 64 bytes
```

The payload following the header appeared as non-readable ciphertext.

This provided runtime evidence that the post-handshake control payload
was no longer transmitted as readable application data.

---

# 18. RTSP and V4L2 Validation

The real camera stream was successfully connected through the complete
media pipeline:

```text
Android camera
      ↓
RTSP over TCP
      ↓
In-process FFmpeg libraries
(libavformat + libavcodec + libswscale)
      ↓
V4L2 loopback
      ↓
/dev/video2
```

The OpenCam V4L2 device was verified as:

```text
card: OpenCam
driver: v4l2 loopback 7.2.7
```

The virtual camera supports formats including YUV420.

A synthetic FFmpeg test successfully wrote:

```text
640×480
30 FPS
YUV420
```

to `/dev/video2`.

The actual Android camera stream was subsequently validated through the
in-process FFmpeg libraries → V4L2 path. The developer later confirmed that
the replacement pipeline remained stable for several minutes and restarted
smoothly.

---

# 19. Media-Pipeline Integration and Troubleshooting

During integration, the media implementation was migrated from an external
FFmpeg process to in-process FFmpeg libraries. A first build attempt encountered
linker errors because the build referenced stale/inconsistent object state;
a clean rebuild succeeded without source changes, and the subsequent test run
passed.

The real Android camera stream was then tested with the new in-process
pipeline. The developer reported:

- live video reached `/dev/video2`
- the stream ran for several minutes without reported problems
- restarting the stream was smooth
- disconnect/shutdown behavior was clean

These are developer-observed integration results, not a claim that every
network, device, or resource-exhaustion edge case has been tested.

---

---

# 20. Automated Testing

## Linux

The Linux test suite was updated alongside the v2 protocol.

Validated areas include:

- mutual proof/session authentication
- session frame authentication
- malformed input handling
- invalid UTF-8/count handling
- RTSP endpoint allowlisting
- shell-metacharacter handling
- media credential derivation

The updated Linux CTest suite completed successfully on rerun:

```text
3/3 tests passed
```

The registered tests were:

- `opencam_security_tests`
- `opencam_shell_injection_regression`
- `opencam_diagnostic_redaction_tests`

The clean build and test rerun were completed after the in-process media
pipeline changes.

---

## Android

The Android test suite was migrated from the earlier protocol version
to the encrypted v2 implementation.

Tests cover:

- token validation
- fragmented handshake handling
- authentication interoperability
- wrong-secret rejection
- full-duplex authenticated communication
- RTSP credential derivation
- AES-GCM tamper detection

The tamper test constructs an authenticated encrypted frame, modifies
ciphertext, and verifies that the receiver rejects the modified frame.

The Android test suite completed successfully after the v2 migration.

---

# 20A. Latest Post-Migration Verification

After the in-process media pipeline was integrated, the developer reported
that a clean build succeeded and the three registered Linux tests passed.
Real-device verification then confirmed that live video worked, the stream
remained stable for several minutes, stream restart was smooth, and
disconnect/shutdown checks were clean.

A second-terminal CTest run passed while the OpenCam session was running.
Process inspection showed only the OpenCam executable, with no separate
FFmpeg or FFprobe process. Source inspection confirmed that the CMake target
includes `src/media_pipeline.cpp` and does not compile `src/ffmpeg_runner.cpp`.

The developer committed and pushed the change as:

```text
81eb515 Fix RTSP credential exposure in media pipeline
```

This is the commit identifier reported by the developer; the report does not
independently query GitHub to verify the remote commit.

---

# 21. Current M3 Status

| Area | Status |
|---|---|
| QR-based pairing | 🟢 Done |
| Mutual authentication | 🟢 Done |
| AES-256-GCM control encryption | 🟢 Done |
| HKDF-SHA256 key derivation | 🟢 Done |
| Directional session keys | 🟢 Done |
| Sequence/replay protection | 🟢 Done |
| Bounded protocol parsing | 🟢 Done |
| RTSP endpoint validation | 🟢 Done |
| Shell-injection remediation | 🟢 Done |
| Android v2 protocol integration | 🟢 Done |
| Linux/Android automated tests | 🟢 Done |
| Real-device Wi-Fi integration | 🟢 Done |
| Encrypted traffic runtime verification | 🟢 Done |
| In-process FFmpeg media pipeline | 🟢 Done and live-tested |
| FFmpeg log suppression and generic media errors | 🟢 Done |
| RTSP credential in FFmpeg subprocess argv | 🟢 Specific exposure path removed |
| RTSP authentication | 🟡 Partial |
| RTSP media encryption / RTSPS | 🔴 Not implemented |
| Forward secrecy | 🔴 Not implemented |
| Repeated-connection/resource hardening | 🟡 Ongoing |
| Multi-interface/network edge cases | 🟡 Ongoing |
| Windows Wi-Fi security | 📋 Future scope |

---

# 22. Remaining M3 Work

The following items remain before M3 can be considered fully closed. The credential exposure through FFmpeg subprocess arguments has been addressed by the in-process migration; media-plane TLS and broader hardening remain open:

### Media-plane encryption

The current Android RTSP server integration does not expose the
server-side TLS/RTSPS functionality required by OpenCam.

Therefore the RTSP media plane remains unencrypted.

### Residual credential handling

The in-process migration removes the credential from FFmpeg/FFprobe command-line
arguments. The authenticated URL still exists in process memory while streaming,
so host-level access, debugging, and crash-dump handling remain relevant. This
specific fix does not replace a broader secrets-handling review.

### Resource and hostile-peer testing

Additional testing is still required for:

- repeated connection attempts
- malformed peers
- resource exhaustion
- unusual disconnect sequences
- multi-interface systems
- additional lifecycle/error paths

### Forward secrecy

The current PSK-based design does not provide forward secrecy.

This can be evaluated as a future protocol enhancement if the threat
model requires it.

---

# 23. Current Security Position

The current M3 implementation should **not** be described as a fully
secure or fully encrypted webcam system.

The accurate security claim is:

> OpenCam provides QR-based pairing, mutual authentication, AES-256-GCM
> encrypted control communication, validated RTSP endpoints, hardened
> process execution, and a tested Linux ↔ Android Wi-Fi integration.
> The RTSP media plane is not currently TLS-encrypted. The previous
> FFmpeg subprocess-argument credential exposure has been remediated, but
> residual in-memory credential exposure and other hardening work remain.

This distinction is important:

```text
CONTROL PLANE
QR pairing
    ↓
Mutual authentication
    ↓
HKDF session keys
    ↓
AES-256-GCM
    ↓
Sequenced authenticated frames
    ↓
Protected control communication


MEDIA PLANE
Android RTSP server
    ↓
RTSP over TCP
    ↓
FFmpeg
    ↓
V4L2
    ↓
Virtual camera

Current limitation:
RTSP media is not yet protected by TLS/RTSPS.
```

---

# 24. Milestone Outcome

M3 moved OpenCam from an unauthenticated network prototype toward a
substantially hardened Linux ↔ Android system.

The most significant completed security improvements are:

1. **Mutual peer authentication**
2. **AES-256-GCM encrypted control communication**
3. **HKDF-derived directional session keys**
4. **Strict frame sequencing and tamper detection**
5. **RTSP endpoint validation**
6. **Shell-injection remediation**
7. **FFmpeg process and descriptor isolation**
8. **Bounded diagnostics and network parsing**
9. **Android and Linux automated security testing**
10. **Real-device Wi-Fi validation**
11. **Runtime verification of encrypted control traffic**

The remaining limitations are clearly identified rather than hidden,
with media-plane encryption, residual secrets-handling review, and broader
resource/network hardening being the most significant outstanding tasks.

---

# 25. Presentation-Safe Project Description

For a project review or presentation, the current M3 state can be
summarized as:

> **OpenCam is a Linux–Android webcam streaming system with QR-based
> pairing, mutually authenticated control communication, AES-256-GCM
> encrypted control frames, validated RTSP endpoints, hardened FFmpeg
> an in-process FFmpeg media pipeline, and a V4L2 virtual-camera output path.
> Milestone 3 focused on security hardening and real-device Wi-Fi integration.
> Media-plane TLS encryption and several cross-platform/network edge cases
> remain future work.**

---

## Appendix — Key Technical Parameters

| Parameter | Current value |
|---|---|
| Control protocol | OCAM v2 |
| Pairing secret | 256-bit random |
| Control encryption | AES-256-GCM |
| KDF | HKDF-SHA256 |
| Control key size | 256-bit |
| GCM tag | 128-bit |
| Max control payload | 64 KiB |
| Control transport | TCP / IPv4 |
| Control port | 6969 |
| RTSP port | 8554 |
| Media transport | RTSP over TCP |
| Video codec | H.264 |
| Validated video size | 640×480 |
| Virtual camera | `/dev/video2` during validation |
| FFmpeg media integration | In-process libavformat/libavcodec/libswscale |
| FFmpeg diagnostics | Library logging suppressed; generic media errors |
| Media TLS | Not currently implemented |
| Forward secrecy | Not currently implemented |
