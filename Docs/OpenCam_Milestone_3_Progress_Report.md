# OpenCam — Milestone 3 Progress Report
## Security Remediation, Encryption & Real-Device Integration

**Date:** 28 September 2026  
**Project:** OpenCam  
**Scope:** Linux controller ↔ Android camera over Wi-Fi  
**Milestone:** M3 — Security, networking and integration hardening

---

## 1. Executive Summary

Milestone 3 began as a security audit and remediation pass over the Linux controller, Android streaming application, and legacy Windows implementation.

The original implementation had several important security weaknesses, including unauthenticated control traffic, shell-mediated probing, weak endpoint validation, descriptor/parser issues, resource/lifecycle problems, and an unauthenticated/unconfidential RTSP media boundary.

The remediation work substantially improved the architecture.

The current Linux ↔ Android Wi-Fi path now has:

- One-time, short-lived QR pairing credentials.
- Mutual authentication between Linux and Android.
- A versioned, bounded control protocol.
- AES-256-GCM encrypted control-plane frames.
- HKDF-SHA256 session/key derivation.
- Directional encryption keys.
- Strict sequence-number checking.
- RTSP endpoint allowlisting.
- Shell-free FFprobe execution.
- Bounded FFprobe/FFmpeg process lifetimes.
- FFmpeg child-process descriptor isolation.
- Bounded FFmpeg stderr diagnostics.
- Random per-session RTSP paths.
- Session-derived RTSP credentials.
- Real physical-device Wi-Fi integration verification.
- Runtime verification of encrypted control frames using tcpdump.
- Runtime verification that FFmpeg uses its own RTSP socket rather than inherited OpenCam listener/control sockets.
- Linux and Android automated test/build validation.

The remaining security limitation is that the current RTSP media plane is not TLS-encrypted, and the FFmpeg CLI currently receives the RTSP credential as part of its input URL. The latter can expose the credential through the FFmpeg process command line to a sufficiently privileged/local observer. This has been identified rather than hidden, and should be addressed in a future media-pipeline/RTSPS phase.

---

## 2. Original M3 Security Findings

The audit identified, among other issues:

1. Wildcard TCP/6969 control listener.
2. No control-channel authentication or encryption.
3. Shell injection risk in RTSP probing through `std::system`.
4. Unauthenticated media/control behavior.
5. Unbounded descriptor/resource accumulation.
6. Android framing/parser weaknesses.
7. Unsafe legacy Windows deserialization.
8. Heuristic interface/address selection.
9. RTSP server authentication/bind/TLS limitations.
10. Android lifecycle and EOF handling issues.
11. Sensitive information appearing in diagnostic/logging paths.
12. FFmpeg inheriting OpenCam file descriptors.
13. Missing or weak process shutdown/reaping behavior.

The historical audit remains preserved in `M3_SECURITY_AUDIT.md`.

---

## 3. Control Protocol Remediation

### Protocol version

The Wi-Fi control protocol was upgraded to **OCAM protocol v2**.

The protocol now uses:

- `OCAM1` framing/magic.
- Explicit protocol version.
- Message type.
- Bounded 32-bit payload length.
- Maximum payload size of 64 KiB.
- Strict frame parsing.
- Handshake deadlines.
- Sequence numbers for authenticated/encrypted frames.

### Pairing

The QR code now contains:

- Linux IPv4 endpoint.
- Control port 6969.
- 256-bit random pairing secret.

The pairing secret is:

- Short-lived.
- One-use.
- Rotated after successful pairing.

The secret is not logged by OpenCam.

### Mutual authentication

The handshake uses domain-separated HMAC-SHA256 proofs:

- `OpenCam client proof v2`
- `OpenCam server proof v2`

Both sides prove knowledge of the pairing secret before accepting the session.

### Session key derivation

The control session key is derived using HKDF-SHA256 from:

- The pairing secret.
- Server nonce.
- Client nonce.
- Protocol-specific context.

Independent directional keys are then derived for:

- Linux → Android.
- Android → Linux.

---

## 4. Control-Plane Encryption

After authentication, sensitive control messages are encrypted using:

**AES-256-GCM**

Encrypted messages include:

- Descriptor.
- Activation.
- Error reports.

The outer frame header remains visible for framing and routing. The encrypted payload contains:

- Sequence number.
- Ciphertext.
- GCM authentication tag.

The outer header is authenticated as AES-GCM AAD.

Nonce construction uses:

- Direction-specific prefix.
- Monotonically increasing sequence number.

Strict sequence equality prevents replay/reordering of authenticated encrypted frames.

### Runtime verification

This was verified on the real Wi-Fi connection using `tcpdump`.

A captured packet contained:

```text
OCAM
02
05
00000040
```

where:

- `OCAM` = OpenCam protocol marker.
- `02` = protocol v2.
- `05` = activation message type.
- `0x40` = 64-byte encrypted wire payload.

The payload bytes appeared as ciphertext rather than readable descriptor/activation fields.

This provides runtime evidence that the v2 control payload is actually encrypted on the network, not merely implemented in source code.

---

## 5. RTSP Endpoint Security

Linux no longer accepts an arbitrary RTSP URL from an authenticated peer.

The endpoint validator requires the expected RTSP structure and rejects:

- Unsupported schemes.
- Unexpected hosts.
- Unexpected ports.
- Userinfo where not allowed.
- Query strings/fragments.
- Other alternate endpoint forms.
- Invalid/random-path requirements.

The Android stream uses a random per-session RTSP path.

The media credential is derived from the authenticated control session.

---

## 6. Shell Injection Remediation

The original RTSP probe used `std::system()` with a peer-controlled URL.

This was replaced with fork/exec-style argument passing.

The current probe:

- Does not invoke a shell.
- Uses explicit FFprobe arguments.
- Uses a protocol allowlist.
- Uses bounded execution time.
- Terminates and reaps the child if the deadline is exceeded.

A regression test covers shell-metacharacter handling.

Linux CTest passed:

```text
2/2 tests passed
```

including the shell-injection regression test.

---

## 7. FFmpeg Process Isolation

The FFmpeg runner was hardened to:

- Use `fork()` + `execlp()`.
- Set close-on-exec behavior.
- Close unrelated file descriptors in the child.
- Redirect unwanted standard streams.
- Capture stderr through a bounded pipe.
- Terminate FFmpeg with SIGTERM first.
- Escalate to SIGKILL if required.
- Always wait/reap the child.
- Close diagnostic descriptors during cleanup.

### Runtime verification

A real streaming run showed:

```text
OpenCam
  └── FFmpeg
```

The FFmpeg process had its own RTSP socket.

The socket inode did not match OpenCam's:

- TCP/6969 listener.
- Accepted control connection.

This confirmed that FFmpeg did not inherit the OpenCam control/listener sockets.

After normal shutdown, the FFmpeg process and its sockets were gone.

**A-1 process isolation/lifecycle verification: PASS.**

---

## 8. FFmpeg Diagnostics

The original FFmpeg runner discarded stderr, making failures such as exit status `8` impossible to diagnose.

The runner now captures stderr using a non-blocking pipe with a bounded diagnostic buffer.

Diagnostics are sanitized before being exposed to OpenCam.

A deterministic negative test using an invalid RTSP endpoint successfully produced:

```text
Connection refused
Error opening input
```

and OpenCam captured the diagnostic output.

**A-2 diagnostic capture: PASS.**

### Remaining secret-handling limitation

FFmpeg's current RTSP CLI does not expose a dedicated RTSP password option.

The current invocation therefore still contains the RTSP credential in the input URL. This means the credential can appear in:

```text
/proc/<ffmpeg-pid>/cmdline
```

and can also be echoed by FFmpeg into stderr.

The stderr path is being treated as a redaction requirement.

Eliminating the process-command-line exposure completely would require a different media-client architecture, such as an in-process libavformat pipeline or another credential-safe media transport mechanism.

This is intentionally recorded as a residual limitation rather than weakening RTSP authentication.

---

## 9. Real Android ↔ Linux Wi-Fi Integration

A physical Android device was used for the integration test.

The following sequence was verified:

1. Android explicitly selected Wi-Fi mode.
2. USB remained physically connected but did not silently override Wi-Fi mode.
3. Linux generated a fresh OCAM1 QR.
4. Android scanned the QR.
5. Mutual control authentication succeeded.
6. Android sent the authenticated descriptor.
7. Linux validated the descriptor.
8. Linux sent activation.
9. Android started the camera/RTSP server.
10. Linux successfully detected RTSP readiness.
11. Manual bounded FFprobe successfully identified:
    - H.264 video.
    - 640×480 video.
    - AAC audio.
12. Linux launched FFmpeg.
13. The final phone-camera → FFmpeg → V4L2 path was exercised successfully after correcting the FFmpeg invocation issue.

The network path therefore progressed from the original connection failures to a working real-device Wi-Fi streaming path.

---

## 10. V4L2 Validation

The OpenCam virtual camera was verified at:

```text
/dev/video2
```

The device reports the OpenCam v4l2loopback node and supports streaming/output formats including YUV420.

An independent synthetic FFmpeg test succeeded:

```bash
ffmpeg -hide_banner   -f lavfi -i testsrc=size=640x480:rate=30   -t 3   -vf format=yuv420p   -an   -f v4l2   -pix_fmt yuv420p   /dev/video2
```

The test produced 90 frames over 3 seconds.

This established that the V4L2 sink can accept the expected YUV420P output independently of the network path.

---

## 11. Android Changes

Android-side remediation includes:

- OCAM protocol v2 implementation.
- AES-256-GCM encrypted control frames.
- HKDF-SHA256 key derivation.
- Independent send/receive encryption keys.
- Strict sequence handling.
- Fragmented-frame handling.
- Bounded reads.
- Authentication failure handling.
- EOF/cleanup handling.
- Secure QR token parsing.
- Random RTSP session paths.
- Session-derived media credentials.
- RTSP server logging disabled.
- Input validation for descriptor/activation fields.

Android unit tests passed after the v2 migration.

A dedicated tamper test was added and passed, verifying that modification of an encrypted frame causes authentication failure.

---

## 12. Automated Build/Test Evidence

### Linux

```text
cmake -S linux -B linux/build
cmake --build linux/build -j2
ctest --test-dir linux/build --output-on-failure
```

Result:

```text
Linux build: PASS
CTest: 2/2 PASS
```

### Android

```text
./gradlew testDebugUnitTest
./gradlew assembleDebug
```

Result:

```text
Android unit tests: PASS
Debug APK build: PASS
```

The APK was installed on the physical Android device for the Wi-Fi integration test.

---

## 13. Current Security Position

OpenCam's security posture is **substantially improved**, but it should not currently be described as secure against a hostile/untrusted network.

### Completed / strongly verified

- QR-based pairing.
- Mutual control authentication.
- Encrypted control-plane messages.
- AES-256-GCM.
- HKDF-SHA256 key derivation.
- Sequence/replay protection at the frame layer.
- RTSP endpoint validation.
- Shell-free RTSP probing.
- FFmpeg process isolation.
- Bounded child lifecycle.
- Bounded diagnostics.
- Random RTSP paths.
- Session-derived RTSP credentials.
- Real Wi-Fi authentication/integration.
- Runtime encrypted-packet verification.
- Linux/Android automated tests.

### Remaining

- RTSP media is not TLS-encrypted.
- RTSP server-side authorization/bind behavior has library limitations.
- FFmpeg command-line credential exposure remains.
- Complete credential-redacted diagnostics still need to be finalized.
- Repeated connection/resource-stress testing remains incomplete.
- Multi-interface/VPN edge cases remain partially tested.
- Windows/Linux end-to-end support is intentionally not part of the current Wi-Fi scope.

---

## 14. Presentation-Safe Project Claim

For a college review, the accurate claim is:

> **OpenCam is a Linux–Android webcam streaming system with QR-based pairing, mutually authenticated control communication, AES-256-GCM encrypted control frames, validated RTSP endpoints, and a V4L2 virtual-camera output path. The current milestone focuses on security hardening and real-device Wi-Fi integration; RTSP media encryption and some cross-platform/network edge cases remain future work.**

Do **not** claim:

- "Fully secure."
- "Military-grade security."
- "Everything is encrypted."
- "Windows support is complete."
- "RTSPS is implemented."

---

## 15. Recommended Next Steps

These are intentionally deferred so the current milestone can be presented cleanly:

### A-3
Finish credential redaction in diagnostics and document the remaining FFmpeg argv exposure.

### A-4
Exercise repeated connection/disconnection and resource cleanup.

### M3 media security
Investigate RTSPS/TLS or an authenticated encrypted media tunnel.

### M3 robustness
Test slow/malformed peers, repeated connections, network loss, and interface changes within safe limits.

### Future Windows work
Treat Windows as a separate revamp rather than claiming cross-platform parity prematurely.

---

## 16. Final Milestone Assessment

Milestone 3 has progressed from an unauthenticated prototype to a substantially hardened and experimentally validated Linux ↔ Android Wi-Fi architecture.

The most significant achievement is that the security changes were not only implemented but exercised on a **real Android device over a real Wi-Fi connection**, including successful mutual authentication and runtime observation of encrypted control traffic.

The remaining work is primarily refinement and media-plane hardening rather than a return to the original insecure architecture.

**Status: M3 security/integration work substantially complete for the current review scope, with explicitly documented residual work.**

---

## Evidence References

- `M3_SECURITY_AUDIT.md` — original findings, historical runtime evidence, and remediation record.
- `M3_SECURITY_REMEDIATION_REPORT.pdf` — remediation/build/integration evidence and remaining-work record.
- Linux control protocol implementation.
- Linux FFmpeg/FFprobe process runners.
- Android control protocol implementation and tests.
- Physical-device Wi-Fi integration logs and packet capture.
