# OpenCam Security

OpenCam's security work focuses on protecting the Linux ↔ Android control
channel, validating untrusted network input, and reducing the risk of
unsafe process and resource handling.

This document describes the **current development security posture**.
It is not a claim that OpenCam is secure against every network threat.

---

## 🔐 Security Scope

The current validated security scope is:

```text
Android device
      │
      │ Wi-Fi / TCP
      ▼
Linux OpenCam controller
      │
      ├── encrypted control channel
      │
      └── authenticated RTSP media session
```

The current development target is **Linux ↔ Android over Wi-Fi**.

Windows support and other transports are outside the current M3 security
scope and are planned for future work.

---

## 🛡️ Current Security Architecture

### QR-Based Pairing

OpenCam uses a random pairing secret distributed through a QR code.

The pairing secret is:

- 256 bits
- intended for one successful pairing
- rotated after successful pairing
- given a limited lifetime

The secret is not intentionally written to normal application logs.

---

## 🤝 Mutual Authentication

The Linux controller and Android client mutually authenticate during the
control-channel handshake.

The handshake uses:

- a random server nonce
- a random client nonce
- the pairing secret
- HMAC-SHA256 authentication proofs

The proof domains are versioned:

```text
OpenCam client proof v2
OpenCam server proof v2
```

A peer that cannot demonstrate knowledge of the pairing secret is not
allowed to establish an authenticated session.

---

## 🔒 Encrypted Control Channel

After authentication, the control channel uses **AES-256-GCM**.

The current protocol version is:

```text
OCAM protocol v2
```

Encrypted control messages include:

- device descriptors
- activation messages
- error reports

The initial handshake remains visible on the network because it is required
to establish the authenticated encrypted session.

### Key Derivation

Session keys are derived using **HKDF-SHA256**.

The session derivation uses:

```text
salt = server nonce || client nonce
IKM  = pairing secret
info = "OpenCam control v2 session"
```

Independent directional keys are then derived for:

```text
server → client
client → server
```

This prevents the same traffic key from being reused in both directions.

---

## 🔢 Frame Integrity and Replay Protection

Encrypted frames contain a monotonically increasing sequence number.

The sequence number is authenticated as part of the encrypted protocol
construction.

Receivers require the expected sequence number.

Consequently, frames that are:

- reordered
- replayed
- duplicated
- modified

are rejected rather than silently accepted as valid control messages.

AES-GCM authentication also detects ciphertext modification.

---

## 📦 Protocol Validation

The control protocol applies bounded parsing to untrusted network input.

Current protections include:

- protocol-version validation
- message-type validation
- bounded payload lengths
- maximum payload size of 64 KiB
- exact-length reads
- connection and handshake timeouts
- sequence validation
- malformed-frame rejection
- validation of descriptor fields and activation parameters

The Android side also validates fields such as:

- lengths
- UTF-8 strings
- enumerated values
- dimensions
- frame-rate values

---

## 🌐 RTSP Endpoint Validation

The Linux controller does not blindly accept arbitrary RTSP endpoints
supplied by the Android peer.

The current validation restricts the accepted endpoint to the authenticated
peer's expected IPv4 address and RTSP port.

The expected endpoint structure includes:

```text
rtsp://<peer>:8554/live/<random-path>
```

The validation rejects unsupported or ambiguous URL forms, including
userinfo, query/fragment components, alternate schemes, and unexpected
hosts or ports.

---

## 💻 Media Pipeline and Process-Execution Remediation

The earlier media implementation passed an authenticated RTSP URL to external
FFmpeg/FFprobe processes. That design created two distinct concerns: shell
interpretation in the old probe path, and credential visibility in process
arguments when a URL containing userinfo was passed to a media subprocess.

The current implementation replaces the RTSP probe and video-processing
subprocesses with **in-process FFmpeg library calls**:

- RTSP readiness probing uses `libavformat`.
- The media worker opens the authenticated RTSP URL in process memory.
- Video is decoded with `libavcodec`, converted with `libswscale`, and written
  to the V4L2 loopback device.
- The media worker runs in a dedicated thread so the control loop can continue
  handling the authenticated control connection.
- The media URL is not passed as an FFmpeg/FFprobe command-line argument.

The previous media subprocess command-line exposure path is therefore removed
from the active pipeline. This does not encrypt RTSP traffic or eliminate every
possible way sensitive data could be exposed from process memory.

Unrelated process-launch paths remain in the project. The QR display path
invokes `qrencode` with fixed arguments and sends the QR payload over stdin;
the V4L2 loopback loader uses a fixed `modprobe` command. Neither path receives
the authenticated RTSP URL. Those paths should still be reviewed independently
as part of future process-execution hardening.

---

## 📝 Diagnostics and Logging

The in-process media pipeline suppresses FFmpeg library log output to avoid
emitting URL-bearing diagnostic messages. OpenCam reports generic media
pipeline failures rather than echoing peer-provided error text.

The project also includes diagnostic-redaction code and automated tests for
sensitive diagnostic strings. These controls reduce accidental disclosure in
normal logs; they are not a guarantee against exposure through debugging,
crash dumps, process-memory inspection, or other host-level access.

---

## ⚠️ Known Limitations

### 1. RTSP Media Plane Is Not TLS-Encrypted

The current RTSP server implementation used by Android does not provide
the required server-side RTSPS/TLS functionality for the current OpenCam
integration.

Therefore:

- the **control plane is encrypted**
- the **RTSP media plane is not currently TLS-encrypted**

This means video media should not currently be considered confidential
against a network observer with access to the media traffic.

RTSPS/media-plane encryption remains future work.

---

### 2. RTSP Credential Handling

The previous FFmpeg CLI media pipeline placed the authenticated RTSP URL in
the FFmpeg process argument list, making the media credential visible through
`/proc/<pid>/cmdline` and potentially through FFmpeg diagnostics.

The current in-process pipeline removes that specific command-line exposure
path: neither the RTSP probe nor the media decoder is launched as an FFmpeg or
FFprobe subprocess, and FFmpeg library logging is suppressed.

The authenticated URL and derived media credential still exist in process
memory while streaming. The RTSP media plane also remains unencrypted by
TLS/RTSPS. Accordingly, this change should be described as remediation of the
**credential-in-subprocess-arguments issue**, not as complete media-plane
confidentiality or a guarantee against all host-level credential exposure.

---

### 3. No Forward Secrecy

The current control-channel design is based on the QR-delivered pairing
secret.

It does not currently provide forward secrecy.

If a pairing secret is compromised after traffic has been recorded, the
design does not provide the same protection that an ephemeral
key-agreement protocol with forward secrecy would provide.

---

### 4. Resource and Edge-Case Hardening

Additional testing remains for:

- repeated connection attempts
- unusual network failures
- multi-interface environments
- malformed or hostile peers
- descriptor/resource exhaustion
- additional lifecycle and cleanup paths

The current implementation includes bounded parsing and connection
timeouts, but this area remains under active hardening.

---

### 5. Platform Scope

The current M3 security implementation is focused on:

```text
Linux controller ↔ Android client
```

Windows support is not part of the current Wi-Fi security implementation.
A future Windows implementation will require its own security review and
integration with the authenticated protocol.

---

## 🧪 Security Validation

The current development work includes automated and real-device validation.

### Linux Tests

The Linux test suite has covered:

- mutual authentication/session proof
- authenticated session frames
- invalid UTF-8/count handling
- RTSP endpoint allowlisting
- shell-metacharacter handling
- media credential derivation

The Linux CTest suite currently passes the available M3 tests.

### Android Tests

Android tests have covered:

- token validation
- fragmented handshake handling
- authentication interoperability
- wrong-secret rejection
- full-duplex authenticated communication
- AES-GCM tamper detection

The Android test suite currently passes after the v2 protocol migration.

### Real-Device Validation

The Linux ↔ Android implementation has been validated using a real Android
device over Wi-Fi.

The tested flow includes:

```text
QR pairing
    ↓
mutual authentication
    ↓
encrypted control session
    ↓
descriptor exchange
    ↓
activation
    ↓
RTSP startup
    ↓
FFmpeg
    ↓
V4L2 virtual camera
```

A packet capture of the control connection also verified that post-handshake
control payloads appear as ciphertext rather than readable application
data.

---

## 🧪 Latest M3 Media-Pipeline Verification

The updated in-process media pipeline was built and exercised on the development
Linux host with a real Android client. The developer reported that the stream
ran for several minutes without issues, stream restart was smooth, all three
registered Linux CTest tests passed on rerun, and disconnect/shutdown checks
were clean.

The active process inspection showed OpenCam running without a separate
FFmpeg or FFprobe process. Source inspection confirmed that
`linux/CMakeLists.txt` includes `src/media_pipeline.cpp` and does not compile
`src/ffmpeg_runner.cpp`.

These checks support the remediation of the specific credential-in-process-
arguments issue. They are not a comprehensive independent security audit, and
RTSP media-plane TLS remains unimplemented.

---

## 📊 Current Security Status

| Area | Status |
|---|---|
| QR-based pairing | 🟢 Implemented |
| Mutual authentication | 🟢 Implemented |
| AES-256-GCM control encryption | 🟢 Implemented |
| HKDF-SHA256 key derivation | 🟢 Implemented |
| Directional session keys | 🟢 Implemented |
| Sequence/replay protection | 🟢 Implemented |
| Protocol bounds and validation | 🟢 Implemented |
| RTSP endpoint validation | 🟢 Implemented |
| Shell-injection remediation | 🟢 Implemented |
| In-process FFmpeg media pipeline | 🟢 Implemented and live-tested |
| FFmpeg library log suppression and generic errors | 🟢 Implemented |
| Real-device Wi-Fi validation | 🟢 Implemented |
| RTSP media authentication | 🟡 Partial |
| RTSP media encryption / RTSPS | 🔴 Not implemented |
| RTSP credential in FFmpeg subprocess argv | 🟢 Specific exposure path removed |
| Forward secrecy | 🔴 Not implemented |
| Multi-interface edge cases | 🟡 Ongoing |
| Repeated-connection/resource hardening | 🟡 Ongoing |
| Windows Wi-Fi security | 📋 Future scope |

---

## 🎯 Security Position

OpenCam's current M3 work should be described as **security hardening of
the Linux ↔ Android control and pairing architecture**, not as complete
end-to-end media security.

The strongest current guarantees are on the control plane:

```text
Pairing
   ↓
Mutual authentication
   ↓
Session key derivation
   ↓
AES-256-GCM encryption
   ↓
Authenticated + sequenced control frames
```

The media plane remains a separate security boundary and currently lacks
TLS/RTSPS encryption.

---

## 🔭 Future Security Work

Planned security improvements include:

- RTSPS/TLS support for the media plane
- review of residual in-memory credential exposure and host-level threat scenarios
- stronger resource-exhaustion defenses
- broader malformed-peer testing
- multi-interface/network topology testing
- improved lifecycle/error-path coverage
- evaluation of forward-secret key exchange
- security review of future Windows support
- final licensing and release documentation

---

## ⚠️ Responsible Use

OpenCam is an actively developed project and should not currently be
treated as a production-grade secure communications system.

For security-sensitive environments, users should operate OpenCam only on
networks they trust until media-plane encryption and the remaining
hardening work are complete.
