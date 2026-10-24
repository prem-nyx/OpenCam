# OpenCam Security

This document describes the security controls implemented in the current
OpenCam development build, along with limitations that remain. It is a
status document, not a certification or a claim that OpenCam is suitable
for every security-sensitive environment.

## Scope

The currently validated deployment is an Android client communicating
with a Linux controller over Wi-Fi. Windows support and other transports
are outside this validation scope.

OpenCam has two distinct network paths:

-   **Control channel:** pairing, authentication, device descriptor,
    activation, and error messages.
-   **Media channel:** RTSP video delivery from Android to Linux.

Security properties of the control channel must not be assumed to apply
to the media channel.

## Control-channel security

### Pairing and mutual authentication

Pairing uses a randomly generated 256-bit secret delivered through a QR
code. The secret is intended for one successful pairing, has a limited
lifetime, and is rotated after successful pairing. Normal application
logging is not intended to include the secret.

The Linux and Android peers authenticate using server and client nonces,
the pairing secret, and HMAC-SHA256 proofs. The proof domains are
versioned for protocol v2. A peer that cannot produce a valid proof
should not establish an authenticated session.

### Encryption and key derivation

After the handshake, control messages use AES-256-GCM under the OCAM v2
protocol. Session keys are derived using HKDF-SHA256 with the server
nonce and client nonce as salt, the pairing secret as input key
material, and the context string `OpenCam control v2 session`.

Separate directional keys are used for server-to-client and
client-to-server traffic.

### Frame validation and replay resistance

Encrypted frames include a sequence number that is authenticated by the
protocol construction. Receivers enforce the expected sequence number.
Authentication failures, modified ciphertext, and unexpected sequence
values are rejected.

The control protocol also applies bounded parsing and input validation,
including protocol-version and message-type checks, a 64 KiB maximum
payload, exact-length reads, connection and handshake timeouts, and
validation of descriptor and activation fields.

The initial handshake is not encrypted; it establishes the authenticated
session used for subsequent encrypted messages.

## RTSP endpoint validation

The Linux controller validates the endpoint supplied by the Android peer
rather than accepting an arbitrary URL. The current accepted structure
is an RTSP endpoint on the expected peer IPv4 address and port 8554,
with a `/live/` path containing a random identifier.

Unsupported URL forms, including unexpected hosts or ports, user
information, query strings, fragments, and alternate schemes, are
rejected.

``` text
rtsp://<peer>:8554/live/<random-path>
```

## Media pipeline and credential handling

The active Linux pipeline uses in-process FFmpeg libraries for RTSP
readiness probing and media processing. The stream is opened through
`libavformat`, decoded with `libavcodec`, converted with `libswscale`,
and written to the V4L2 loopback device. The media worker runs
separately from the control loop.

This removes the previous exposure path in which the authenticated RTSP
URL was supplied as an FFmpeg or FFprobe subprocess argument. FFmpeg
library logging is suppressed, and the application reports generic media
errors rather than echoing arbitrary peer-provided error text.

This is a targeted remediation, not a guarantee against all credential
exposure. The URL and derived media credential remain in process memory
while streaming. Other fixed-argument process-launch paths, such as QR
display and V4L2 loopback setup, are separate and should be reviewed
independently.

## RTSP authentication in the Android server fork

The OpenCam RTSP server fork adds Basic authentication checks for
supported RTSP methods. Its tests cover missing, incorrect, and
malformed authorization, successful authorization, unknown methods, and
authentication requirements across supported methods.

Basic authentication is an access-control check, not encryption. Base64
encoding does not provide confidentiality. Without TLS, authentication
exchanges and media traffic must not be considered confidential against
a network observer.

The authentication unit tests do not establish TLS support or prove
end-to-end interoperability with every OpenCam build. An
empty-credentials edge case should also be tested explicitly before
treating the implementation as fully hardened.

## Known limitations

### Media transport is not TLS-encrypted

The current OpenCam integration uses `rtsp://`; RTSPS/TLS is not yet
implemented and verified end to end. Video media should therefore not be
considered confidential against an observer able to capture the media
traffic.

Do not change production URLs to `rtsps://` until server-side TLS,
certificate provisioning, certificate/identity verification on Linux,
and end-to-end streaming have been implemented and tested. There must be
no silent plaintext fallback.

### No forward secrecy

The control-channel session keys are derived from the QR pairing secret
and handshake nonces. The current protocol does not provide forward
secrecy through an ephemeral key-agreement exchange.

### Resource and lifecycle hardening

Further testing is needed for repeated connection attempts, hostile or
malformed peers, descriptor/resource exhaustion, multi-interface network
configurations, unusual network failures, and lifecycle/error cleanup
paths.

### Platform scope

The current security implementation is focused on Linux and Android over
Wi-Fi. A future Windows implementation requires its own integration and
security validation.

## Validation recorded during development

The development record reports the following Linux coverage: session
proofs, authenticated frames, malformed UTF-8/count handling, RTSP
endpoint allowlisting, shell-metacharacter handling, and media
credential derivation. The Linux CTest suite was reported passing.

Android coverage includes token validation, fragmented handshake
handling, authentication interoperability, wrong-secret rejection,
full-duplex authenticated communication, and AES-GCM tamper detection.
The Android test suite was reported passing after the v2 protocol
migration.

Real-device testing on Wi-Fi has exercised QR pairing, mutual
authentication, encrypted control exchange, activation, RTSP startup,
and video delivery into a V4L2 virtual camera. A packet capture
reportedly showed post-handshake control payloads as ciphertext. The
updated in-process media pipeline was also reported to run for several
minutes, with clean stream restart and shutdown checks.

The RTSP fork's `:rtspserver:testDebugUnitTest` task was reported
`BUILD SUCCESSFUL`. These are development test results, not an
independent audit or a guarantee that every failure mode has been
tested.

## Current status

  -----------------------------------------------------------------------
  Area                                Status
  ----------------------------------- -----------------------------------
  QR pairing and mutual control       Implemented
  authentication                      

  AES-256-GCM control encryption      Implemented

  HKDF-SHA256 and directional keys    Implemented

  Sequence validation and bounded     Implemented
  protocol parsing                    

  RTSP endpoint validation            Implemented

  Removal of RTSP URL from FFmpeg     Implemented
  subprocess arguments                

  In-process FFmpeg pipeline and      Implemented and live-tested during
  reduced diagnostics                 development

  RTSP Basic-authentication hardening Implemented; unit tests reported
  in fork                             passing

  End-to-end RTSPS/TLS                Not implemented

  Forward secrecy                     Not implemented

  Broader resource and hostile-peer   Ongoing
  testing                             

  Windows security integration        Future work
  -----------------------------------------------------------------------

## Planned security work

1.  Implement server-side RTSPS/TLS support in the Android streaming
    stack.
2.  Provision the server certificate or trust material through the
    authenticated encrypted control channel.
3.  Verify the certificate and server identity on Linux.
4.  Update both readiness probing and the media pipeline to use the same
    TLS verification policy.
5.  Test successful streaming, invalid certificates, wrong identities,
    expired/untrusted certificates, and connection failures.
6.  Ensure TLS failures stop the connection rather than falling back to
    plaintext RTSP.
7.  Add resource-exhaustion, lifecycle, malformed-peer, and
    multi-interface tests.
8.  Evaluate a forward-secret key exchange for the control protocol.
9.  Complete licensing and dependency attribution before a formal
    release.

## Responsible use

OpenCam remains under active development. Until RTSPS/TLS and the
remaining hardening work are complete, use it on trusted networks and do
not treat the current implementation as a production-grade secure
communications system.
