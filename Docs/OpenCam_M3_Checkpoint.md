# OpenCam Development Checkpoint

**Purpose:** Record the repository and implementation state at the
current M3 pause point.

## Repositories

-   OpenCam: <https://github.com/prem-nyx/OpenCam>
-   OpenCam RTSP server fork: <https://github.com/prem-nyx/RTSP-Server>
-   Upstream RTSP server: <https://github.com/pedroSG94/RTSP-Server>

The OpenCam repository uses `main` as its default branch. The RTSP fork
uses `master` as its default branch and retains the
`opencam/auth-hardening` feature branch.

## Completed work recorded

-   Linux and Android control protocol v2 with mutual authentication.
-   AES-256-GCM encrypted control messages and HKDF-SHA256 directional
    keys.
-   Pairing secret and nonce-based handshake.
-   Bounded protocol parsing and RTSP endpoint validation.
-   In-process FFmpeg RTSP probing and media processing, removing the
    prior URL-in-subprocess-arguments exposure path.
-   Diagnostic reduction and generic media errors.
-   RTSP server fork authentication hardening and unit tests.
-   Development validation on a real Android device and Linux V4L2
    loopback.

## Current security boundary

The control channel is authenticated and encrypted. The media channel
still uses RTSP without TLS. RTSP Basic authentication does not encrypt
media traffic. Do not claim end-to-end media confidentiality or switch
to `rtsps://` until TLS and identity verification are implemented and
tested.

## Next work when development resumes

1.  Review the RTSP authentication edge cases, including explicit
    authorization with empty server credentials.
2.  Confirm compatibility with the final Android app build.
3.  Implement RTSPS/TLS support in the Android RTSP server.
4.  Provision trust material over the encrypted control channel.
5.  Validate the server certificate and identity on Linux.
6.  Apply the same TLS verification policy to the readiness probe and
    media pipeline.
7.  Test valid and invalid certificates, connection failures, restart,
    and shutdown.
8.  Confirm there is no plaintext fallback.
9.  Update `README.md`, `SECURITY.md`, and test reports only after
    verifying the resulting implementation.

## Freeze policy

While M3 is paused, avoid unreviewed code changes, dependency upgrades,
merges, and unnecessary pushes. Keep documentation claims tied to
implementation and test evidence.
