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

## 💻 Shell-Injection Remediation

An earlier implementation invoked external RTSP probing through a shell,
allowing peer-controlled RTSP URL data to reach `std::system()`.

This was identified as a high-severity issue during the M3 security audit.

The current implementation uses direct `fork()` / `exec()` argument passing
for FFprobe instead of shell command construction.

FFmpeg is likewise launched through direct process execution.

This prevents RTSP URL metacharacters from being interpreted as shell syntax.

---

## 🧰 FFmpeg Process Isolation

The Linux controller launches FFmpeg as a separate child process.

Current hardening includes:

- direct `exec()` invocation
- close-on-exec handling
- closing inherited file descriptors
- controlled stdin/stdout/stderr handling
- bounded process shutdown
- SIGTERM followed by SIGKILL if required
- explicit child reaping

Real-device testing also verified that the FFmpeg RTSP socket is a
separate socket from OpenCam's control listener.

---

## 📝 Diagnostics and Logging

FFmpeg stderr is captured through a bounded non-blocking diagnostic pipe.

Diagnostic output is capped to prevent unbounded accumulation.

OpenCam's normal textual logging does not intentionally print:

- pairing secrets
- RTSP passwords
- authenticated URLs

However, the current FFmpeg command-line media pipeline has an important
credential-exposure limitation described below.

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

### 2. RTSP Credential Exposure in FFmpeg

The Linux media pipeline currently constructs an authenticated RTSP URL
containing the media credential before invoking FFmpeg.

As a consequence, the credential can be visible in:

```text
/proc/<ffmpeg-pid>/cmdline
```

FFmpeg may also reproduce the URL in diagnostic stderr when an RTSP
connection fails.

OpenCam's diagnostic handling should therefore not be considered a
complete solution to this issue.

A future credential-safe media pipeline is required to remove this
command-line exposure. One possible architectural direction is replacing
the credential-bearing FFmpeg CLI invocation with an in-process media
pipeline or another mechanism that does not place credentials in process
arguments.

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
| FFmpeg process isolation | 🟢 Implemented |
| FFmpeg diagnostic capture | 🟢 Implemented |
| Real-device Wi-Fi validation | 🟢 Implemented |
| RTSP media authentication | 🟡 Partial |
| RTSP media encryption / RTSPS | 🔴 Not implemented |
| FFmpeg credential exposure | 🟡 Known limitation |
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
- removal of media credentials from process command lines
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
