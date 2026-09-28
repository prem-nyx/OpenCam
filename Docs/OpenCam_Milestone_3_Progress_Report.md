# OpenCam — Milestone 3 Progress Report

**Date:** 28 September 2026  
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
- FFmpeg process isolation
- file-descriptor isolation
- bounded FFmpeg diagnostics
- Android protocol and validation hardening
- real-device Wi-Fi integration
- end-to-end RTSP → FFmpeg → V4L2 validation
- security-focused automated testing
- runtime packet-capture verification of encrypted control traffic

The current M3 implementation is **substantially complete for the
Linux ↔ Android Wi-Fi control-plane scope**.

The major remaining security limitation is that the RTSP media plane is
not currently protected by TLS/RTSPS. There is also a known credential
exposure issue caused by the current FFmpeg command-line media pipeline.

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

# 12. Shell-Injection Remediation

The earlier implementation used `std::system()` for RTSP probing.

Because the RTSP URL originated from peer-controlled data, this created a
shell-injection risk.

The RTSP probe was redesigned to use direct process execution:

```text
fork()
  ↓
exec()
  ↓
ffprobe
```

The URL is passed as an argument rather than embedded inside a shell
command.

This removes shell interpretation from the RTSP probing path.

The FFmpeg runner uses the same direct process-execution approach.

---

# 13. FFmpeg Process Isolation

FFmpeg is launched as a child process of the OpenCam Linux controller.

The process-launch path now includes:

- direct `exec()` invocation
- close-on-exec handling
- closing inherited file descriptors
- controlled stdin/stdout/stderr
- bounded shutdown
- SIGTERM followed by SIGKILL when required
- explicit process reaping

A real-device process inspection was performed during streaming.

The resulting process relationship was:

```text
OpenCam
   └── ffmpeg
```

The FFmpeg RTSP socket was confirmed to be separate from the OpenCam
control listener.

The OpenCam control listener remained owned by the OpenCam process,
while FFmpeg owned its own RTSP connection socket.

After normal shutdown, the FFmpeg process and associated sockets were
confirmed to be gone.

This provides runtime evidence for both process isolation and normal
lifecycle cleanup.

---

# 14. FFmpeg Diagnostics

FFmpeg stderr is now captured through a bounded non-blocking diagnostic
pipe.

Current diagnostic handling includes:

- non-blocking reads
- bounded diagnostic storage
- an 8192-byte diagnostic limit
- process cleanup
- stderr capture for connection and media failures

A deterministic failing-connection test successfully produced useful
FFmpeg diagnostics instead of silently hiding the underlying failure.

This significantly improves troubleshooting of RTSP and V4L2 failures.

---

# 15. Credential Exposure Investigation

During the M3 security review, the actual FFmpeg process was inspected.

The RTSP credential was found to be present in:

```text
/proc/<ffmpeg-pid>/cmdline
```

A deterministic test also demonstrated that when an RTSP URL containing
a test credential was supplied to FFmpeg, the credential could appear
verbatim in FFmpeg stderr.

For example, the diagnostic path reproduced the supplied test credential
inside the FFmpeg error message.

### Current assessment

This is a known security limitation of the current media architecture.

The control-plane authentication and encryption are not weakened by this
finding, but the media credential itself can be exposed through the
FFmpeg CLI process representation.

A complete fix likely requires a credential-safe media integration that
does not place the RTSP password in the process command line.

---

# 16. Real-Device Wi-Fi Integration

The updated implementation was tested using an actual Android device
and Linux host over Wi-Fi.

The verified sequence was:

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
RTSP
      ↓
FFmpeg
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
same FFmpeg → V4L2 path.

---

# 19. FFmpeg/V4L2 Troubleshooting

During real-device integration, FFmpeg initially failed with a non-zero
status.

The failure investigation included:

- exposing FFmpeg stderr
- validating the RTSP endpoint independently
- testing the V4L2 device
- verifying supported pixel formats
- checking installed FFmpeg/FFprobe capabilities
- testing a synthetic V4L2 source
- correcting the FFmpeg invocation

A separate test also confirmed that an intentionally invalid RTSP
endpoint produced a clear connection-refused diagnostic.

The final pipeline successfully reached the virtual camera.

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

The Linux CTest suite completed successfully:

```text
2/2 tests passed
```

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
| FFmpeg process isolation | 🟢 Done |
| FFmpeg diagnostic capture | 🟢 Done |
| FFmpeg credential exposure | 🟡 Known limitation |
| RTSP authentication | 🟡 Partial |
| RTSP media encryption / RTSPS | 🔴 Not implemented |
| Forward secrecy | 🔴 Not implemented |
| Repeated-connection/resource hardening | 🟡 Ongoing |
| Multi-interface/network edge cases | 🟡 Ongoing |
| Windows Wi-Fi security | 📋 Future scope |

---

# 22. Remaining M3 Work

The following items remain before M3 can be considered fully closed:

### Media-plane encryption

The current Android RTSP server integration does not expose the
server-side TLS/RTSPS functionality required by OpenCam.

Therefore the RTSP media plane remains unencrypted.

### Credential-safe media process

The current FFmpeg CLI invocation places the RTSP credential in the
process argument list.

A future media integration should remove this exposure.

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
> The RTSP media plane is not currently TLS-encrypted, and the current
> FFmpeg media pipeline has a known credential-exposure limitation.

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
with media-plane encryption and credential-safe FFmpeg integration being
the most significant outstanding security tasks.

---

# 25. Presentation-Safe Project Description

For a project review or presentation, the current M3 state can be
summarized as:

> **OpenCam is a Linux–Android webcam streaming system with QR-based
> pairing, mutually authenticated control communication, AES-256-GCM
> encrypted control frames, validated RTSP endpoints, hardened FFmpeg
> process execution, and a V4L2 virtual-camera output path. Milestone 3
> focused on security hardening and real-device Wi-Fi integration.
> Media-plane TLS encryption and several cross-platform/network edge
> cases remain future work.**

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
| FFmpeg | External child process |
| FFmpeg diagnostics | Bounded stderr capture |
| Media TLS | Not currently implemented |
| Forward secrecy | Not currently implemented |
