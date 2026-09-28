# OpenCam — Milestone 3 Security & Network Audit

**Assessment date:** 2026-09-27  
**Method:** Static repository inspection, with limited safe local runtime inspection.  
**Scope note:** The active M1/M2 path is the Linux C++ controller and Android app. The repository also contains a substantial legacy Windows controller; it is included below and kept distinct from Linux behavior. No source or configuration changes were made for this investigation.

## 1. Executive Summary

The current Linux control server listens on every IPv4 interface at TCP/6969 and accepts an Android descriptor without authentication, encryption, or authorization. Its QR code contains only the selected IPv4 endpoint and port. The descriptor's RTSP URL is then used by `ffprobe` and FFmpeg. In `linux/src/rtsp_probe.cpp::waitForRtsp`, that URL is interpolated into a shell command passed to `std::system`; a descriptor crafted by any peer that can reach the control service can therefore reach a shell-injection sink. This is a **confirmed high-severity finding** (M3-01).

Other confirmed risks include unbounded descriptor accumulation and blocking in the single-threaded Linux accept loop (M3-03), Android's unframed parsing of arbitrary TCP read chunks and missing length checks (M3-04), and unchecked Windows descriptor/error-report deserialization (M3-05). The Linux FFmpeg invocation uses `fork`/`execlp` with separate arguments, so that particular invocation has no shell metacharacter interpretation; however, its URL remains peer-controlled and protocol-unrestricted (M3-02).

There is no pairing identity or cryptographic authentication in the source. RTSP is a separate media boundary. Android constructs a server on port 8554 and starts streaming at `/live`, without application code configuring credentials or TLS. Whether the third-party RTSP library binds all interfaces and permits anonymous clients must be confirmed against its implementation/runtime; it is not elevated to a source-confirmed claim here. Windows also accepts control connections on IPv4 wildcard port 6969 and has a separate RTSP client path.

**Current security posture:** suitable only for a trusted, isolated test network with the applications used interactively. Do not treat QR scanning as authentication or assume a firewall, localhost, or the advertised address narrows the listener.

## 2. Repository Scope

The repository inventory included root documentation and scripts; all Linux C++ sources and CMake configuration; Android manifest, Gradle configuration, Kotlin/Java sources, resources, tests, and dependency declarations; and Windows project configuration and source for networking, serialization, RTSP/FFmpeg, ADB, settings, GUI, virtual-camera, logging, and installation/package support. Binary images, icons, APK-independent binary assets, bundled ADB executables/DLLs, and the Gradle wrapper JAR were inventoried by path, not reverse-engineered.

Primary evidence is repository source. Milestone reports and README describe intended/previously tested behavior, but claims in those documents were checked against implementation where possible. No build or security tests were run. The one pre-existing staged change was `README.md`; it was left untouched.

Relevant dependency declarations include AndroidX CameraX 1.4.2, ML Kit Barcode 17.3.0, Pedro RootEncoder 2.6.1 and RTSP-Server 1.3.6 (`android/app/build.gradle.kts`); Linux uses system `ffmpeg`, `ffprobe`, `qrencode`, `modprobe`, and V4L2 loopback by executable name/command (`linux/src/*`); Windows uses Asio, FFmpeg libraries, wxWidgets, and bundled ADB (`windows/VCamdroid.vcxproj`, `windows/src/*`). Dependency versions/build artifacts beyond declarations were not independently audited.

## 3. Architecture

### Current Linux + Android architecture

```text
Android camera / CameraX
        │
        ├── TCP control client, initiated by Android ──────────────┐
        │   descriptor → Linux; activation ← Linux                 │
        │                                                          ▼
        │                                                Linux OpenCam controller
        │                                                TCP server 0.0.0.0:6969
        │                                                          │
        │                         supplied RTSP URL ────────────────┤
        │                                                          ├─ ffprobe client (readiness)
        │                                                          └─ FFmpeg client
        │                                                                 │
        └── Android RTSP server :8554/live ◀──── RTSP media ─────────────┘
                                                                          │
                                                                    /dev/video*
                                                                V4L2 loopback
                                                                          │
                                                                  OBS / applications
```

The control and media paths are distinct. Android initiates TCP control to the address encoded in the QR. It sends a serialized `DeviceDescriptor`, which includes its RTSP URL and capability lists. Linux sends an activation packet; Android then starts its RTSP server/stream. Linux probes the supplied URL, then launches FFmpeg to read it and write fixed 640×480 YUV420P output to the discovered OpenCam V4L2 device. The client role on the media path is Linux (`ffprobe`, then FFmpeg); the RTSP server is Android (`Streamer.kt`, backed by the declared RTSP-Server library).

### Legacy Windows path

```text
Android ── TCP control ──▶ Windows Asio server :6969
Android RTSP server :8554 ◀── Windows FFmpeg-library RTSP receiver
                                  │
                                  ├── preview/UI
                                  └── DirectShow virtual-camera source

Windows startup additionally invokes adb reverse (6969) and adb forward (8554).
```

Windows' control server is separate from Linux. It reads a descriptor once with `read_some`, deserializes it, then its UI can send activation and controls. Its RTSP receiver opens the URL in FFmpeg libraries and requests UDP by default, switching to TCP if the URL text contains `127.0.0.1`. Windows includes ADB forwarding/reversing code; Linux does not. The Android source has a USB/ADB localhost connection path, but no Linux ADB transport implementation was found. USB/ADB is therefore not the implemented Linux M3 path.

No file-descriptor passing or other OS descriptor exchange was found. Descriptor exchange means the application-level `DeviceDescriptor` byte structure.

## 4. Network Attack Surface

| Service/path | Protocol, role, port/bind | Initiator and purpose | Authentication / encryption / authorization | Reachability and interface notes |
|---|---|---|---|---|
| Linux control | TCP server, IPv4 `0.0.0.0:6969`, backlog 4 (`linux/src/main.cpp::main`) | Android connects using QR endpoint; descriptor and activation/control lifecycle | None in source; cleartext; any accepted peer can submit a descriptor | All local IPv4 interfaces: Wi-Fi, Ethernet, VPN, Docker/Podman, libvirt bridges, etc., subject to host firewall. Not IPv6 (`AF_INET`). Advertised address is separately chosen by first non-loopback IPv4 found. |
| Android control | TCP client, ephemeral local port → QR-specified IPv4/port (`TCPConnection`) | Android initiates connection after QR scan/confirmation; USB code connects to `127.0.0.1:6969` | No TLS, server identity check, or challenge in app code | Outbound client socket; remote endpoint is supplied by the scanned QR. QR validation accepts a dotted IPv4 regex and integer port conversion, but does not establish trust. |
| Android RTSP | RTSP server constructed on port 8554; advertised as `rtsp://<control-socket-local-address>:8554/live` (`Streamer`) | Linux `ffprobe` then FFmpeg connects after activation | App passes no credentials/token/TLS configuration. Library defaults and precise bind address are **UNKNOWN** from repository source. | Expected to be reachable from other devices on the connected LAN if library binds a wildcard interface; confirm library bind behavior/runtime. Stream is only requested after control activation. IPv6 behavior unknown. |
| Linux media clients | `ffprobe` and FFmpeg as clients; RTSP transport forced to TCP | Linux connects to peer-supplied URL for readiness and capture | No media authentication configured by OpenCam; URL could embed userinfo but that value is peer-controlled and logged | Outbound endpoint/protocol derives from descriptor URL; this is an SSRF-like network access boundary. FFmpeg may support schemes beyond RTSP unless restricted by its build/options. |
| Windows control | TCP server `tcp::v4()` port 6969, wildcard IPv4 (`windows/src/net/server.cpp::Server`) | Android connects; accepts descriptor then receives commands/errors | No app-level authentication/encryption/authorization | Reachable on IPv4 interfaces subject to Windows Firewall. IPv6 is not requested. Listener bind is not constrained to the QR-selected address. |
| Windows media client | FFmpeg library client; descriptor URL; UDP default, TCP if URL contains `127.0.0.1` | Windows opens Android RTSP URL on user selection | No credentials/TLS setup in app code | Remote URL is supplied by unauthenticated control peer; UDP RTSP can create dynamic media sockets. Exact socket range and filters depend on FFmpeg/OS. |
| ADB path (Windows) | `adb reverse tcp:6969 tcp:6969`; `adb forward tcp:8554 tcp:8554` | Windows starts mappings; Android USB path dials loopback; Windows RTSP path uses forwarded loopback | ADB transport authorization is Android/ADB's own mechanism, outside the app protocol; no app identity | Port mappings are created by `windows/src/net/server.cpp` and `adb.h`; dynamic/local listeners should be checked at runtime. No equivalent Linux setup exists in source. |
| Dormant UDP wrapper | Android `UDPConnection` source is fully commented out | None | Not applicable | Does not create a service in current code. |

Other network-related behavior: Windows `GetHostInfo` opens a UDP socket to `8.8.8.8:80` only to learn the OS-selected local source address; this is an outbound route-selection probe, not a listening service. Linux uses no UDP discovery or multicast. No Bluetooth discovery, mDNS, or LAN broadcast implementation was found. V4L2 is local device I/O, not a network service.

## 5. Control-Plane Analysis

### Linux/Android protocol

1. Linux computes an IPv4 string and displays `address:6969` as text QR (`linux/src/main.cpp`, `network.cpp`, `qr.cpp`).
2. Android scanner splits on `:` and accepts exactly two components; address must match its IPv4 regex and port is parsed with `toInt` (`QRScanner.parseResult`). User confirms the endpoint in `MainActivity`.
3. Android creates a blocking `Socket(ipAddress, port)` on an IO coroutine and starts one reader thread (`TCPConnection`). There is no explicit connect/read timeout or TLS.
4. Android sends a big-endian descriptor: length-prefixed UTF-8 model name and RTSP URL, 16-bit counts and 16-bit width/height pairs, then filter count/name/category (`DeviceDescriptor.serialize`). There is no top-level packet length or message type for this initial descriptor.
5. Linux appends socket bytes to a vector and retries `parseDeviceDescriptor` until parsing succeeds. Parser checks available bytes for fields and rejects trailing bytes, but caller treats every exception as “incomplete”, has no maximum, and has no deadline.
6. Linux discovers/loads V4L2, returns `ACTIVATION` (0x02) with fixed defaults (30 FPS, 640×480, back camera, bitrates, boolean fields, focus, zero filters/effect). This packet's fixed format is in `linux/src/stream_options.cpp`.
7. Android interprets the first byte as type. Activation deserializes fixed fields and invokes camera preview/stream startup; other type codes adjust camera, filters, bitrate, FPS, zoom, etc. Linux's current M2 loop does not parse incoming control messages after activation; it reads one byte only to detect disconnect.
8. No client/server identity, shared secret, pairing token, TLS, certificate checks, challenge response, replay counter, or authorization check is present. The control connection itself acts as the only sequencing context.

The descriptor has internal 16-bit lengths/counts, but no whole-message length or bounded maximum. The Linux parser's checks prevent ordinary out-of-bounds reads for incomplete descriptor data, but do not bound cumulative allocation or waiting. Integer fields are decoded as unsigned 16-bit values; no semantic checks reject empty/oversized names, invalid URL, zero/huge resolutions, unknown filter category, or unreasonable counts. The Android descriptor serializer converts sizes to `Short`; local device capability lists are the source, but values beyond the wire range would wrap.

### Legacy Windows protocol

Windows uses the same rough descriptor layout but `Server::TCPDoAccept` assumes one `read_some` returns the complete descriptor and passes the byte count to a deserializer that does not check it. Windows activation serialization also differs from Android's current deserializer: Windows writes booleans as 32-bit integers and omits the focus field, while Android reads one-byte booleans and a 32-bit focus field. This is a compatibility/state parsing defect and can misalign subsequent values. Windows parses later device error reports from each 255-byte async read without message framing/size validation.

### Authentication and authorization answers

- A reachable arbitrary client can open Linux/Windows control TCP. The server does not authenticate it.
- A connecting Android sends its descriptor automatically; Linux activates it once V4L2 is available. No policy asks whether that device was paired before.
- A rogue Linux endpoint can be selected by a forged/replaced QR. Android sends its model/capability/RTSP endpoint descriptor and accepts control packets from that peer, including camera activation/settings. QR scanning confirms an endpoint string, not an identity.
- Linux does not authorize protocol types or device capabilities beyond parsing descriptor layout. It currently does not consume post-activation operations from peer to host, but it accepts one connection at a time and processes the peer's descriptor and URL.
- Commands from the control server can request camera activation and adjustments, including flash, camera selection, codec, filters and stream parameters (Android `StreamActivity`/`Streamer`). There is no protocol authentication on those commands.

## 6. Media-Plane / RTSP Analysis

### Control plane

The control plane is TCP/6969 on Linux/Windows. It has no cryptographic peer authentication, encryption, or device authorization. Descriptor and activation bytes, device model, endpoint and capability data are visible to an on-path LAN observer.

### Media plane

The media server is Android, via `RtspServerCamera2` constructed with port 8554. `Streamer.URL` uses `/live`; its descriptor URL is generated from the TCP socket's local address plus that fixed port/path. Linux separately probes then consumes the received URL. No stream token, credential, TLS, or RTSP authentication setup is present in app code. The path and port are predictable. Since the URL is sent in cleartext in the descriptor, a LAN observer can learn it.

Whether port 8554 binds `0.0.0.0` or another Android interface remains **UNKNOWN from checked-in source**. No server-side authentication configuration is passed. During active streaming, an unauthenticated `OPTIONS` and `DESCRIBE` request from the Linux control host received `200 OK` responses (Section 22). This confirms anonymous RTSP protocol access from that host, but not media retrieval by an independent LAN client, Android bind scope, or access after control disconnect.

Linux FFmpeg is given the received URL as `-i` after the `ffprobe` readiness check. `execlp` receives it as a single argument, so shell metacharacters do not become a shell command there. The URL remains unvalidated and may choose an unexpected FFmpeg input protocol or target. Media confidentiality/integrity is not provided by RTSP in this code path.

## 7. QR Pairing & Trust Model

Linux QR payload is exactly `<chosen IPv4 address>:6969`; Windows QR does the same from its host-info tuple. No secret, nonce, device identity, public key, certificate, signature, session token, or cryptographic material is encoded. Android checks address syntax and converts the port, then shows the endpoint in a confirmation dialog.

**Does possessing/scanning the QR prove device identity? No.** It merely tells Android where to connect. An attacker able to replace, imitate, or supply a QR can redirect Android to an attacker-controlled endpoint; QR possession is not proof that the endpoint is the intended OpenCam host.

## 8. IP / Interface Handling

### Linux

`linux/src/network.cpp::getLocalIpAddress` iterates `getifaddrs` in returned order, considers only `AF_INET`, skips only the exact `127.0.0.1` address, and returns the first remaining address. It does not inspect interface-up/running flags, choose a route toward Android, prefer Wi-Fi/Ethernet, exclude VPN/containers/virtual bridges, enumerate candidates, or support IPv6. This function is called once before QR display. The server separately binds `INADDR_ANY` at IPv4 port 6969.

Concrete failures: the first address may be Docker/libvirt/VPN instead of the LAN route; a disconnected/stale or unusual loopback address may be selected; Android on a different subnet cannot reach the advertised endpoint; changing Wi-Fi/routes after startup leaves a stale QR; an IPv6-only client/network cannot use this server. Regardless of which address is advertised, all IPv4 interfaces are bound. The first-address policy is unsuitable for a multi-interface host and can expose control service on unintended interfaces.

### Android

For Wi-Fi pairing, the RTSP host in descriptor comes from `socket.localAddress` for the route used to the QR-selected endpoint. This is more route-sensitive than Linux's `getifaddrs` first-address choice but is not identity validation and can still change on reconnect/network handoff. The USB/ADB loopback case intentionally yields a local endpoint; Windows configures ADB forwarding, Linux does not.

### Windows

Windows host address discovery uses a dummy UDP `connect` to `8.8.8.8:80` then reads the selected local endpoint (`Server::GetHostInfo`). This typically follows the OS default route, but the external target is hard-coded, requires route-selection behavior, may be blocked, and falls back to `127.0.0.1`, which is unusable for a remote Wi-Fi phone. It does not prove Android reachability. Listener binding is all IPv4 regardless of the advertised address.

The current host's observed interfaces, routes, and wildcard listener are recorded in Section 22. The multi-interface selection behavior remains untested because only one non-loopback interface was UP.

## 9. FFmpeg Security

Linux has two subprocess paths:

- `linux/src/rtsp_probe.cpp::waitForRtsp` constructs a shell command string with the network-supplied URL between double quotes and invokes `std::system`. Quotes inside the URL can terminate the quoting and add shell syntax. This is a confirmed command-injection sink reachable from an unauthenticated TCP descriptor (M3-01).
- `linux/src/ffmpeg_runner.cpp::startFfmpeg` forks and calls `execlp("ffmpeg", ..., "-i", rtspUrl.c_str(), ..., videoDevice.c_str(), nullptr)`. Arguments are passed directly, not through a shell, so shell metacharacters are not command injection here. However the URL is not parsed/allowlisted and reaches FFmpeg as a protocol/endpoint selector; arbitrary schemes/hosts and local or internal resource access may be possible depending on installed FFmpeg protocols. Treat as a likely SSRF/local-resource boundary, not proven arbitrary file read/write by this source alone.
- `linux/src/v4l2_device.cpp::loadV4L2Loopback` invokes a fixed `modprobe` command through `system`; its string contains no peer-controlled content. It runs with OpenCam's OS privileges and may fail without sufficient privileges. No injection path from descriptor reaches this command.
- QR generation uses `fork`/`execlp` with fixed arguments and payload as one argv value; no shell.

Windows RTSP `Receiver::OpenConnection` passes descriptor URL to `avformat_open_input`, without a shell but also without a protocol/host allowlist. Windows `adb.h` constructs `system` strings from the executable directory and integer port constants; those values are local installation/configuration inputs, not the network descriptor. Paths with spaces/metacharacters may break invocation; no network-to-shell flow was established there.

## 10. Input Validation

### Linux descriptor path

Bounds checks in `linux/src/device_descriptor.cpp` verify individual reads against the accumulated vector. The caller (`linux/src/main.cpp::handleClient`) catches all parser exceptions as “incomplete,” appends every received byte, and loops indefinitely until parse or EOF. Consequences: memory grows without a maximum; a peer can hold the only serving thread by connecting and sending nothing/partial data; a malformed descriptor never gets a distinct rejection path. There are no read deadlines or semantic constraints. The parser has no independent cap on total bytes, list counts, string values beyond the wire's 16-bit per-field bound, resolution dimensions, URL scheme/host/port, or filter category. No demonstrated C++ buffer overflow was found in this Linux parser itself.

### Android control packet path

`TCPConnection.startReceiveBytesLoop` reads arbitrary TCP chunks into a reusable 512-byte buffer, then passes both buffer and returned byte count. `StreamActivity.onBytesReceived` ignores `bytes`, assumes one complete command starts at index 0, and reads indexes/string lengths without validating the actual chunk length. TCP may fragment or coalesce messages. `StreamOptions.deserialize` parses fixed fields and 16-bit filter counts/lengths from the whole buffer without checking a protocol length; malformed data can throw underflow/index exceptions, and large counts can consume CPU before failing. Simple commands may read stale bytes from the previous read. Effects include incorrect camera settings and app exceptions/DoS from a peer selected via QR. Numeric values such as resolution, FPS, bitrate, zoom and rotation are not range-checked before camera/encoder APIs. Filter names are mapped against a fixed repository, which limits arbitrary class selection, but adjustment values are not clamped.

The receive loop treats EOF as disconnect only for `isOverAdb`. On Wi-Fi, `read()` returning -1 is forwarded to the callback and the loop continues; this can repeatedly process stale data and spin rather than cleaning up. There are no socket read/write deadlines. `close()` joins the reader thread after closing the socket; lifecycle behavior needs device testing.

### Windows parser path

`windows/src/net/server.cpp::TCPDoAccept` does one `read_some` into 512 bytes and calls `DeserializeDeviceDescriptor`; `size` is passed but ignored in `windows/src/net/serializer.cpp`. The serializer reads offsets and lengths without any bounds check. A short/malformed descriptor can trigger out-of-bounds reads/undefined behavior; large declared counts can drive loops and allocations from memory read past the provided buffer. Error-report deserialization also ignores `size`. This is confirmed unsafe parsing in the legacy Windows listener. Async subsequent control/error reads likewise have no explicit framing, and Windows `Connection::Read` treats each up-to-255-byte read as one error report.

No source evidence of an integer overflow or arbitrary code execution in these parsers was established beyond memory-unsafe reads and denial/crash potential. Runtime behavior and exploitability are **UNKNOWN**.

## 11. Resource Exhaustion

- Linux backlog is 4, but only one connection is accepted/processed at a time in a blocking loop. A single idle/slow descriptor blocks every other client; the OS backlog is not an application connection limit.
- Linux descriptor vector has no cap; socket reads have no timeout. Unauthenticated memory and connection-slot exhaustion are realistic.
- Once accepted, readiness checking can launch up to 10 sequential `ffprobe` shell processes with 500 ms sleeps. A successful injected command can bypass expected behavior; even without injection, a peer can consume wait/probe time. The read itself has no process/network deadline except retries.
- Linux starts one FFmpeg child; disconnect sends SIGTERM and waits, but no timeout/escalation if child does not exit. Early failure branches rely on returning and closing client socket; a child only exists after `startFfmpeg`. Process exit/signal cleanup behavior has not been tested.
- Android creates a new coroutine/socket/reader thread per connect attempt; no explicit connect/read timeout or global attempt guard at manager level. Camera/stream parameters from activation have no range validation. It has a fixed 512-byte receive buffer, but that does not constitute framing or size validation.
- Android RTSP session/connection limits and bitrate enforcement depend on the external library and are **UNKNOWN** from this code.
- Windows accepts asynchronously and stores each connection; no app-level maximum or handshake timeout. It allocates 255-byte buffers per connection and RTSP receiver threads on stream selection/retry. Receiver retries every 500 ms, and FFmpeg interrupt logic has a three-second activity timeout, but session count/connection caps are absent. Descriptor sizes/counts are untrusted.
- No explicit OpenCam rate limit, max concurrent client policy, CPU/memory quota, FFmpeg protocol allowlist, or stream bandwidth cap is implemented for Linux/Android control.

## 12. Connection Lifecycle

```text
Android scans QR → TCP connect → descriptor send
       → Linux parses descriptor → verifies/loads V4L2
       → Linux sends activation → Android parses options and starts RTSP
       → Linux ffprobe readiness check → FFmpeg starts → writes V4L2
       → control EOF/error → Linux SIGTERM + wait → accept next client
```

The Linux loop is sequential and has basic cleanup on the established streaming path. If the Android peer disconnects during descriptor receive, Linux returns and closes the socket. If RTSP probing fails, it does not start FFmpeg; it returns to accept. If the control socket fails after FFmpeg starts, Linux stops/waits for that child. A stopped process, network loss during probing, or a child ignoring SIGTERM may stall cleanup/accept; no cleanup timeout exists. Linux ignores content on control channel after activation, so duplicate activation/control state is not handled there.

Android `StreamActivity.onDisconnected` stops streamer and finishes. But Wi-Fi EOF is not detected as a disconnect by `TCPConnection`; exceptions do notify. Android stream stop is tied to UI surface/lifecycle as well as explicit disconnect; process death cleanup is library/OS dependent. No retry/reconnect state machine is explicit in `ConnectionManager`; on successful connect it starts StreamActivity and the QR scanner stops. MainActivity's `onResume` can restart camera and initiate Wi-Fi connect again if its camera field remains set, so lifecycle/re-entry deserves device verification. The `ConnectionManager` is singleton and assigns `tcpConn` without closing any previous connection first; duplicate attempts can leave stale sockets/readers. `streamingEnabled` is declared but unused.

Windows removes disconnected connections from its vector but has rough concurrency/lifecycle handling: synchronous descriptor read occurs inside the Asio accept callback; parser failure/exception handling is not robust; `Close` stops context and closes connections but ADB cleanup removes reverse mapping and kills ADB server. Windows UI RTSP manager's streaming index and descriptor erasure can become inconsistent if devices disconnect while a stream is selected. RTSP worker retries on failure; its stop callback aborts blocking reads after inactivity. This is a legacy reliability concern, not part of Linux M2 path.

No descriptor or RTSP URL is cryptographically bound to the authenticated identity of the control peer because no such identity exists. Reconnects create no fresh token/nonce and there is no replay protection. The stream URL remains fixed/predictable across sessions.

## 13. Logging / Information Disclosure

Normal Linux logs include peer IPv4 address, device model/name, RTSP URL, descriptor byte counts, V4L2 device path, FFmpeg PID and lifecycle/errors (`linux/src/main.cpp`). Windows logs remote endpoint, device name, stream URL, errors and lifecycle details (`windows/src/net/*`, `windows/src/rtsp/*`). Android logs QR-selected IP/port, connection lifecycle, camera/stream errors and writes app logs under private internal storage; `LogActivity` lets the user share the file through a `FileProvider` with URI read grant. These are **harmless-to-potentially-sensitive diagnostic data** on a trusted local machine but reveal IPs, device model, local device paths, stream endpoints and software behavior. If a peer supplies RTSP userinfo credentials in the URL, printing the full URL can expose them in logs; the app-generated URL currently has no credentials.

No fixed long-lived secret or token is logged because no such credential is implemented. The Android manifest exports `LogActivity` and `MainActivity`; `StreamActivity` and FileProvider are not exported. `LogActivity` displays logs but does not itself return/share the file without a user tap. `FileProvider` exposes the app's internal files path only through granted URI access; the chooser sends the log file to the selected sharing target. Android `allowBackup=true` is set and backup rule files contain no active excludes; whether this private log is included in device backup depends on Android backup behavior and should be checked on supported versions. The report does not treat that as a confirmed leak.

## 14. Threat Model

### Threat A — Same-LAN attacker

**Linux host:** Can attempt TCP/6969 on any IPv4 interface. If OpenCam is running and firewall permits access, source accepts one peer without auth. The attacker can stall descriptor parsing, consume memory, submit an arbitrary RTSP URL, trigger ffprobe/FFmpeg behavior, and reach the confirmed shell injection path. They can observe cleartext control descriptor/URL. **Android media:** Once active, direct access to :8554 may be possible; library bind/default auth requires runtime confirmation. No Linux multicast discovery was found.

**Windows host:** TCP/6969 is wildcard IPv4 and unauthenticated; Windows parser is unsafe. Windows Firewall may block inbound access but is not enforced by OpenCam. Android RTSP access from another LAN peer is a separate library behavior.

### Threat B — Rogue Android client

Can connect to Linux control if it can reach TCP/6969 without QR possession. It can supply an arbitrary descriptor and RTSP URL; no identity or prior pairing check exists. Linux will respond with activation after V4L2 checks and attempt the supplied URL. As M3 Linux ignores later command bytes, the client does not gain a host command interpreter via post-activation control parsing; the URL-to-shell flow remains critical. Windows may crash/behave unpredictably on malformed descriptor and can follow the supplied stream URL.

### Threat C — Rogue Linux client

If a user scans a QR pointing at the rogue host, Android will connect and automatically send a descriptor. The rogue server can send activation/control packets; Android has no server identity validation and allows camera capture/stream operations through protocol handlers. A rogue control server could command flash/camera/stream settings. Whether rogue host can view stream depends on the RTSP server's bind/access behavior; the rogue host is on the connection's route and knows the advertised URL.

### Threat D — Malformed protocol client

Linux's bounds-checked parser rejects incomplete field accesses by throwing, but caller keeps appending and waits forever with no size/deadline, causing memory/availability denial. Android's handler ignores actual bytes-read count and trusts command-specific indexes/lengths; invalid/fragmented input can cause exceptions, stale-byte operations, or EOF spinning. Windows descriptor/error parsers perform unchecked reads outside provided buffer bounds, giving confirmed crash/undefined-behavior potential. Exploitability beyond denial/crash is unknown.

### Threat E — Direct RTSP attacker

RTSP is not gated by control authorization in the app configuration: Linux connects directly to the URL, and Android app has no credential/TLS configuration. Runtime from the Linux host confirmed unauthenticated OPTIONS/DESCRIBE and SDP access at :8554 while the stream was active. Whether a separate LAN device can complete SETUP/PLAY and retrieve media, and which Android interfaces bind, remain **UNKNOWN**.

### Threat F — Multi-interface host

Linux advertises first eligible `getifaddrs` IPv4 but binds all IPv4 interfaces. It can advertise a VPN/container/virtual bridge address while still exposing port 6969 on Wi-Fi/Ethernet/other bridges. Windows chooses an outbound default-route source for the QR but binds all IPv4. Neither uses the QR address to constrain bind. IPv6 listeners are not explicitly created, but Android RTSP library IPv6 behavior is unknown.

## 15. Security Findings

Severity assesses impact and reachability in the implemented defaults. Status says what source proves; runtime-only behavior is not promoted to confirmed.

### M3-01 — Shell injection through peer-supplied RTSP URL

- **Severity / status:** HIGH — **CONFIRMED**
- **Affected component:** Linux readiness probe; `linux/src/main.cpp::handleClient` → `waitForRtsp`; `linux/src/rtsp_probe.cpp::waitForRtsp`, lines 21–30.
- **Precondition:** Attacker can connect to TCP/6969 and submit a descriptor that parses; OpenCam reaches the RTSP probe (V4L2 available or load succeeds).
- **Technical explanation:** `descriptor.rtspUrl` is network-controlled and concatenated between double quotes in a shell command passed to `std::system`. An embedded quote can terminate shell quoting and append commands. The separate FFmpeg `execlp` path is not shell-mediated, but the probe runs first.
- **Impact:** Code execution as the OpenCam Linux user's OS account, with that account's local permissions; likely compromise of user data and camera host. Not automatically root; `modprobe` is a separate fixed command and may require privileges.
- **Evidence:** `linux/src/main.cpp:84-93, 220-228`; `linux/src/rtsp_probe.cpp:21-30`; descriptor URL parser `linux/src/device_descriptor.cpp:111-123`.
- **Mitigation:** Remove shell construction: execute ffprobe via `fork`/`exec` argv or a safe process API, impose a strict RTSP URL parser and `rtsp` scheme/host/port policy, and never accept a URL from an unauthenticated peer as executable input.
- **Complexity:** Low–medium.
- **Runtime verification:** Not required to establish the source injection path; use an isolated local harness after fix to verify no shell metacharacters execute.

### M3-02 — Unauthenticated peer controls media endpoint and protocol selection

- **Severity / status:** MEDIUM — **CONFIRMED control trust and local non-RTSP ffprobe handling; LIKELY broader network access**
- **Affected component:** Linux `handleClient` / `waitForRtsp` / `startFfmpeg`; Windows `Server::TCPDoAccept` / `RTSP::Receiver::OpenConnection`.
- **Precondition:** Reach control TCP and send a descriptor (or have user scan a forged QR for Android-to-rogue-server case).
- **Technical explanation:** No client identity/authentication exists. A complete descriptor with `file:///dev/null` from an unauthenticated local TCP client caused Linux to send its activation packet and spawn `ffprobe`; the installed ffprobe recognizes the `file` URL and reports invalid media data. Descriptor URL also flows to FFmpeg `-i`; Windows passes URL to `avformat_open_input`. Linux does not constrain URL scheme/host/port. `execlp` avoids shell injection but does not enforce URL policy. No arbitrary remote/internal endpoint was accessed.
- **Impact:** Unauthenticated camera activation/control and use of the host's network/media stack to connect to attacker-chosen/internal services; exact non-RTSP protocol and file effects depend on installed FFmpeg protocols. Windows may likewise access endpoints via libavformat.
- **Evidence:** `linux/src/main.cpp:84-93, 220-228`; `linux/src/ffmpeg_runner.cpp:8-40`; `windows/src/net/server.cpp:126-151`; `windows/src/rtsp/receiver.cpp::OpenConnection`.
- **Mitigation:** Authenticate peers before descriptor acceptance; require a narrowly defined RTSP URL with allowed address/port and no userinfo; restrict FFmpeg protocols and network destinations; do not infer authorization from a descriptor.
- **Complexity:** Medium, coupled to pairing/auth design.
- **Runtime verification:** Local file-scheme handling and ordinary RTSP operation were observed. Broader scheme acceptance, destination reachability, and exact SSRF impact still require isolated testing.

### M3-03 — Linux unauthenticated descriptor path permits memory and connection denial of service

- **Severity / status:** MEDIUM — **CONFIRMED BY SOURCE AND RUNTIME**
- **Affected component:** Linux TCP server and descriptor parser; `handleClient`, `parseDeviceDescriptor`.
- **Precondition:** Reach TCP/6969.
- **Technical explanation:** One accepted connection is handled synchronously. Incomplete/malformed parse exceptions are swallowed; each read appends to an unbounded vector, with no timeout, maximum descriptor size, or semantic count limits. Runtime tests confirmed that a no-QR client can connect and a 3-byte descriptor prefix remained open for at least 1.2 seconds; another client's TCP handshake queued behind it and proceeded after the first closed. Bounded incomplete payloads through 256 KiB and a huge declared count did not crash the process, but the server did not reject them before client closure. This does not bound larger or prolonged inputs.
- **Impact:** OpenCam process memory exhaustion and denial of service to legitimate pairing/streaming.
- **Evidence:** `linux/src/main.cpp:36-95, 420-463`; `linux/src/device_descriptor.cpp:30-107` has only per-read bounds checks.
- **Mitigation:** Add a strict total descriptor cap and field/count limits; use explicit frame length and parser result states (incomplete/invalid/complete); set handshake read deadline, reject malformed input, and isolate/limit concurrent clients.
- **Complexity:** Medium.
- **Runtime verification:** Bounded idle, queue-starvation, incomplete, and oversized-prefix behavior confirmed. Longer duration, sustained memory growth, and actual exhaustion were intentionally not tested.

### M3-04 — Android control parser ignores TCP framing and supplied byte count

- **Severity / status:** MEDIUM — **CONFIRMED**
- **Affected component:** Android `TCPConnection.startReceiveBytesLoop`; `StreamActivity.onBytesReceived`; `StreamOptions.deserialize`.
- **Precondition:** Android connects to a malicious or malformed control endpoint, commonly through a substituted QR; or a legitimate peer sends fragmented/coalesced/malformed packets.
- **Technical explanation:** TCP `read` chunks are not packet boundaries. Handler ignores `bytes`, reuses 512-byte buffer, indexes/decodes fields without validating lengths, and activation parser reads counts/lengths without protocol framing. Malformed data can cause exceptions; fragmented data can be misread using stale bytes. On Wi-Fi EOF, -1 is not treated as disconnect, and the loop can repeatedly dispatch stale buffer content.
- **Impact:** Android app crash, CPU spin, incorrect camera actions/settings, or stream instability. Remote code execution is not demonstrated.
- **Evidence:** `TCPConnection.kt:59-75`; `StreamActivity.kt:63-129`; `StreamOptions.kt::deserialize`.
- **Mitigation:** Define one framed protocol with length/type/version; buffer and parse exact complete messages; reject invalid lengths/types/order and values; handle EOF identically for all transports; add read timeout and exception boundary that closes session.
- **Complexity:** Medium.
- **Runtime verification:** Yes, including device-side safe fragmentation, coalescing, malformed and EOF tests.

### M3-05 — Windows legacy descriptor/error deserialization reads beyond received bytes

- **Severity / status:** MEDIUM — **CONFIRMED memory-unsafe parser; exploit impact UNKNOWN**
- **Affected component:** Windows control server; `Server::TCPDoAccept`, `Serializer::DeserializeDeviceDescriptor`, `Serializer::DeserializeErrorReport`.
- **Precondition:** Reach Windows TCP/6969 and send a short or malformed descriptor, or send malformed error data on an accepted connection.
- **Technical explanation:** One `read_some` result (up to 512 bytes) is passed to deserializer, but `size` is unused. Length/count offsets are read without bounds checks, including category byte reads. Error-report deserializer also ignores its size. This is out-of-bounds access/undefined behavior; reliable exploitation beyond crash is not established.
- **Impact:** Windows controller crash or memory-unsafe behavior; possible but unproven further compromise.
- **Evidence:** `windows/src/net/server.cpp:126-151, 172-175`; `windows/src/net/serializer.cpp:40-111, 149-161`.
- **Mitigation:** Retire/replace parser with checked cursor over a fully framed, capped message; treat incomplete vs invalid distinctly; catch all parse errors and close peer before UI callbacks.
- **Complexity:** Medium.
- **Runtime verification:** Yes for sanitizer/fuzz-style parser tests and local malformed-input regression tests; no aggressive live fuzzing on production service.

### M3-06 — Control service binds all IPv4 interfaces while QR advertises a heuristic address

- **Severity / status:** MEDIUM — **CONFIRMED bind/selection behavior; actual exposure depends on host policy**
- **Affected component:** Linux `main`/`getLocalIpAddress`; Windows `Server`/`GetHostInfo`.
- **Precondition:** OpenCam runs on a host with multiple IPv4 interfaces.
- **Technical explanation:** Linux selects first non-`127.0.0.1` IPv4 from `getifaddrs`, but binds `INADDR_ANY`. Windows selects source address for a dummy UDP route to `8.8.8.8` but binds `tcp::v4()` wildcard. QR does not constrain listener. VPN/container/virtual interfaces can be advertised or expose service unexpectedly; failures include wrong subnet and stale endpoint.
- **Impact:** Unintended interface exposure and failed/redirected pairing. The unauthenticated service findings increase impact.
- **Evidence:** `linux/src/network.cpp:10-58`; `linux/src/main.cpp:336-405`; `windows/src/net/server.cpp:13-19, 33-55`.
- **Mitigation:** Select interface/address explicitly or bind the intended LAN address; show all candidates and require user selection when ambiguous; refresh on route change; support IPv6 deliberately or reject/document it.
- **Complexity:** Medium.
- **Runtime verification:** Yes on Wi-Fi/Ethernet/VPN/container/bridge setups and after interface changes.

### M3-07 — Android RTSP accepts unauthenticated protocol requests; bind scope remains unknown

- **Severity / status:** MEDIUM — **CONFIRMED unauthenticated OPTIONS/DESCRIBE from Linux host; bind scope and independent media access UNKNOWN**
- **Affected component:** Android RTSP server `Streamer`; RTSP-Server dependency.
- **Precondition:** Stream active and server bound to a reachable interface; attacker can route to phone port 8554.
- **Technical explanation:** App constructs RTSP server at fixed port 8554 and path `/live`, with no authentication credentials, token, TLS or access-control configuration. While the normal stream was active, a separate Linux-host TCP client sent unauthenticated RTSP `OPTIONS` and `DESCRIBE`; both returned `RTSP/1.0 200 OK`, and DESCRIBE returned SDP. This client did not request media. Android-side bind scope and independent second-device access remain unknown. After the active control/media session ended, a later TCP connect to `10.229.94.172:8554` was refused; Android process state was unavailable, so this does not isolate whether the app stopped the server or the device became otherwise unavailable.
- **Impact:** Anonymous callers on a path-reachable interface can query RTSP capabilities and stream metadata. Actual unauthorized media retrieval/confidentiality loss is plausible but was not dynamically demonstrated in this test.
- **Evidence:** `Streamer.kt:37-45, 291-302`; dependency version `android/app/build.gradle.kts`; runtime OPTIONS/DESCRIBE responses documented in Section 22.
- **Mitigation:** Require per-session unguessable stream credentials/token and enforce them at RTSP server; bind only required interface where feasible; use authenticated encrypted transport if supported or tunnel media over authenticated session. Test actual DESCRIBE/SETUP/PLAY policy after implementing access control.
- **Complexity:** Medium–high, dependency/API dependent.
- **Runtime verification:** Active host-side OPTIONS/DESCRIBE are confirmed; Android bind address and second-device media access remain required tests.

### M3-08 — Android control socket EOF/connect lifecycle lacks consistent timeout and cleanup

- **Severity / status:** LOW — **CONFIRMED behavior; impact depends on lifecycle**
- **Affected component:** Android `TCPConnection`, `ConnectionManager`, `MainActivity`.
- **Precondition:** Server closes a Wi-Fi control socket or endpoint is slow/unresponsive; repeated connection flow.
- **Technical explanation:** Socket constructor uses no connect timeout. EOF -1 is handled only when `isOverAdb`; Wi-Fi read loop continues. `ConnectionManager.connect` replaces singleton `tcpConn` without closing previous connection and no explicit reconnect/backoff exists.
- **Impact:** Stuck/spinning reader thread, stale socket/resource leakage, duplicate stream/session state.
- **Evidence:** `TCPConnection.kt:35-48, 59-82`; `ConnectionManager.kt:68-94, 113-121`.
- **Mitigation:** Apply connect/read deadlines, handle EOF on all transports, serialize connection state, close prior session before replace, and make cleanup idempotent.
- **Complexity:** Low–medium.
- **Runtime verification:** Yes with controlled local disconnect/reconnect and process/background lifecycle checks.

### M3-09 — Stream URL and host details are written to logs

- **Severity / status:** LOW — **CONFIRMED logging; credential sensitivity conditional**
- **Affected component:** Linux/Windows logging and Android diagnostics.
- **Precondition:** Local logs are exposed/shared, or descriptor URL contains userinfo credentials.
- **Technical explanation:** Linux prints descriptor URL; Windows logs RTSP URLs and remote endpoints. Android logs endpoint address/port and connection details. An attacker-controlled URL could carry userinfo that is then logged; normal app-generated URL has no credential.
- **Impact:** Disclosure of LAN addresses, device model/paths/endpoints; possible credential disclosure only if credentials are added/supplied in URL.
- **Evidence:** `linux/src/main.cpp:97-108`; `windows/src/rtsp/manager.cpp::Connect2Stream`; `TCPConnection.kt:42`; `Logger.kt` stores internal log file and `LogActivity` offers explicit share.
- **Mitigation:** Redact URL userinfo/query secrets and sanitize control characters before terminal/UI logging; keep log sharing explicit and scoped.
- **Complexity:** Low.
- **Runtime verification:** Not required for current log statements; verify redaction after change.

### M3-10 — Legacy Windows activation packet disagrees with Android decoder

- **Severity / status:** MEDIUM — **CONFIRMED protocol mismatch**
- **Affected component:** Windows `Serializer::SerializeStreamOptions`; Android `StreamOptions.deserialize`.
- **Precondition:** Use current Windows controller with this Android client and activate/configure stream.
- **Technical explanation:** Windows writes boolean values as four bytes and omits focus mode; Android reads one-byte booleans and then a 32-bit focus field. Subsequent filter fields are misaligned. This can yield incorrect values or parse failures.
- **Impact:** Stream activation/configuration failure or unstable Android camera parameters. Security impact is secondary but makes protocol state unpredictable.
- **Evidence:** `windows/src/net/serializer.cpp:114-146`; `android/.../rtsp/StreamOptions.kt::deserialize`.
- **Mitigation:** Define one versioned canonical schema and share serialization fixtures across platforms; validate exact packet length and values.
- **Complexity:** Medium.
- **Runtime verification:** Yes with cross-platform golden packet tests.

### M3-11 — FFmpeg inherits OpenCam listener and accepted control descriptors

- **Severity / status:** MEDIUM — **CONFIRMED inheritance at runtime; orphan/restart impact LIKELY, not directly tested**
- **Affected component:** Linux FFmpeg process creation; `linux/src/main.cpp` socket setup and `linux/src/ffmpeg_runner.cpp::startFfmpeg`.
- **Precondition:** A descriptor reaches FFmpeg startup; OpenCam's listening and accepted sockets are open when it forks.
- **Technical explanation:** `fork` inherits descriptors 3 and 4, and the child calls `execlp` without closing them or setting close-on-exec. Runtime `/proc/<pid>/fd` and `fdinfo` inspection showed FFmpeg retained the same listening socket and accepted control socket as OpenCam, with flags indicating no `FD_CLOEXEC`. If the parent exits while FFmpeg remains alive, these references can keep the listener and control connection open, delaying/rejecting restart and leaving stale session state. Parent-death behavior was not induced.
- **Impact:** Potential stale port ownership, delayed control disconnect, orphaned media bridge, and inability to restart cleanly after an abnormal parent exit.
- **Evidence:** `linux/src/ffmpeg_runner.cpp::startFfmpeg` forks/execs without descriptor cleanup; Section 22 records matching socket inodes and flags for live parent/child.
- **Mitigation:** Set `FD_CLOEXEC` on listening/accepted sockets or explicitly close unrelated descriptors in the child before exec; ensure parent-death and shutdown paths terminate/reap the child with bounded waits.
- **Complexity:** Low.
- **Runtime verification:** Inheritance confirmed. Parent-exit/orphan behavior remains untested because it would interrupt the local session; verify later in an isolated harness.

## 16. Runtime Verification

The initial runtime snapshot (preserved below as a historical snapshot) was followed by the bounded active checks documented in the latest Section 22. Tests used only the local OpenCam listener, the known Android endpoint on the user's private LAN, and a harmless `file:///dev/null` probe. No shell-injection payload, external destination, aggressive fuzzing, or uncontrolled resource test was performed. The current Section 22 supersedes conflicting statements in the historical snapshot.

The latest runtime checks confirmed wildcard IPv4 bind, unauthenticated descriptor acceptance through activation/ffprobe, queue starvation by an incomplete client, anonymous RTSP OPTIONS/DESCRIBE while active, local file-scheme handling by ffprobe, and FFmpeg descriptor inheritance. Android parser tests, second-device access, Android bind scope, reconnect, and parent-exit behavior remain unverified.

## 17. Recommended Security Architecture

Keep the two-plane design but bind both planes to a pairing session:

1. Pairing QR contains an endpoint plus a high-entropy, short-lived one-time pairing secret (or public-key fingerprint). User confirms a short authentication phrase/fingerprint on both devices. QR is not reusable after pairing.
2. Use a small authenticated handshake: client proves possession of the one-time secret; both sides derive a fresh session key with a vetted TLS/Noise-style library. Prefer TLS with pinned/self-issued identity if it fits Android and Linux dependencies; validate the expected key from the QR. This prevents same-LAN observation/modification and rogue endpoint substitution. Plain TLS without pinning or a trusted certificate check does not solve rogue-host pairing.
3. Bind the authenticated peer identity to a session token and authorize only the expected descriptor, activation and documented setting messages. Use a versioned, length-prefixed message format, strict parser, monotonic sequence numbers/nonces, and explicit state transitions. This addresses injection through unauthorized descriptors, replay, parser desynchronization and command authorization.
4. Replace shell-based `ffprobe` with a direct argv process invocation. Permit only RTSP URL syntax and approved destinations/ports; reject userinfo and non-RTSP schemes. Keep process launch arguments fixed and apply process/time/resource limits.
5. Protect media separately. Best simple option is RTSP over a tunnel secured by the same authenticated session, if practical. Otherwise assign a fresh random per-session media credential/token, require it at the Android RTSP server, use encrypted media transport where supported, and revoke it on control disconnect. Confirm server bind behavior and choose an explicit interface. A token in a plain RTSP URL is bearer access, so TLS/tunnel or trusted isolated transport is needed against LAN sniffing.
6. Bind control listener to the selected interface/address rather than wildcard by default, with explicit opt-in for other interfaces. Make address selection route-aware and display interface name/address before pairing; support IPv6 only as a deliberate tested mode.

This avoids treating QR as a secret identity by itself, avoids custom cryptography, and addresses the concrete endpoint substitution, LAN observation, URL injection, and unauthorized camera activation threats. A full mutual TLS PKI may be unnecessary for a personal device pair; short-lived QR-authorized key pinning can provide the needed identity with less operational burden.

## 18. Prioritized Implementation Roadmap

| Phase | Work and likely files | Architectural impact / dependencies | Complexity | Testing and migration |
|---|---|---|---|---|
| **Phase 1 — Immediate security fixes** | Remove `std::system` ffprobe URL construction (`linux/src/rtsp_probe.cpp`); strict RTSP URL/protocol/destination validation (`linux/src/device_descriptor.*`, Linux client path); cap descriptor size/counts and add deadlines (`linux/src/main.cpp`, parser); Android framed parser, lengths, EOF and numeric bounds (`TCPConnection.kt`, `StreamActivity.kt`, `StreamOptions.kt`); Windows parser guards if Windows remains supported (`windows/src/net/server.cpp`, `serializer.cpp`). | No pairing migration needed for code-execution and parser bounds. Keep protocol compatible temporarily only if strict framing can be introduced behind a version bump. | Low–medium Linux shell fix; medium parsers/timeouts. | Safe local unit/golden malformed cases; verify network restrictions and FFmpeg protocol allowlist. Existing clients with malformed/legacy packet layouts may be rejected. |
| **Phase 2 — Authentication and pairing** | Linux QR and handshake (`linux/src/main.cpp`, `qr.*`); Android QR result/connection manager (`QRScanner.kt`, `MainActivity.kt`, `ConnectionManager.kt`, `TCPConnection.kt`); Windows `QrconView`, `Server`, `Connection`, ADB flow if maintained. Add small vetted TLS/pinning or session handshake dependency. | Adds explicit pair identity, one-time code/key and versioned authenticated control. Re-pair both apps; old clients rejected or clearly offered legacy mode only on explicit opt-in. | Medium–high. | Wrong token, replay, endpoint substitution, MITM simulation on isolated LAN, key rotation, reconnect tests. |
| **Phase 3 — Media-plane security** | Android RTSP setup (`Streamer.kt`) and RTSP library integration; Linux probe/FFmpeg client (`rtsp_probe.cpp`, `ffmpeg_runner.cpp`); Windows receiver if supported. | Per-session media auth plus encryption/tunnel; session-bound random credential and revoke on disconnect. Dependency capability may force a transport/library change. | Medium–high. | Direct access without token denied; stale token denied; credential not exposed on LAN; verify bind address, UDP/TCP and stream shutdown. Migration requires coordinated Android/Linux release. |
| **Phase 4 — Hardening** | Interface selection/bind controls (`network.cpp`, Linux `main.cpp`; Windows `GetHostInfo`/Server); state machine and resource caps (`ConnectionManager`, `StreamActivity`, `main.cpp`); process lifecycle (`ffmpeg_runner.cpp`); log redaction (`Logger.kt`, Linux/Windows logger). | Restricts exposed interfaces, memory/time/process/network resources and stale session state. | Medium. | Multi-interface/VPN/container/IPv6 matrix; process kill/failure; resource boundary and reconnect tests. Defaults should avoid broad exposure. |
| **Phase 5 — Security regression testing** | Add protocol fixtures/tests for Linux and Android; Windows if supported; manual plan below. CI/build definitions (`linux/CMakeLists.txt`, Android Gradle and Windows project) only as needed. | Keeps shared schema and security expectations aligned across platforms. | Medium. | Cross-platform golden serialization tests, parser boundary tests, local integration test server, release checklist. No fuzzing on deployed LAN devices. |

## 19. Security Test Plan

Run only against local test builds/devices on an isolated network. Use a controllable local mock peer; do not scan unrelated hosts.

1. **Unauthorized Linux client:** Connect without a valid pairing proof; expect rejection before descriptor parsing, camera activation, process creation, or V4L2 writes.
2. **Unauthorized Android client:** Attempt to connect to Linux from a new unpaired device; expect no descriptor acceptance or activation.
3. **Wrong token/key:** Test empty, altered, expired and wrong-device credentials; verify constant behavior and no media start.
4. **Malformed control packet:** Test unknown type, too-short fields, invalid UTF-8, invalid enum/category, trailing bytes and invalid message ordering; expect bounded reject/close, no crash.
5. **Oversized message:** Send total size above configured maximum and maximum field/count values; verify early close and bounded memory/CPU.
6. **Invalid RTSP URL:** Try non-RTSP scheme, userinfo, malformed host/port, loopback/internal destinations, shell quote/metacharacter text and IPv6 literals. Verify validation rejects; assert no shell child or unrelated endpoint connection is made.
7. **Direct RTSP access:** While active and inactive, connect from Android itself, paired Linux, and a second trusted LAN test device. Without current session credentials expect refusal; with valid session expect only authorized access. Check IPv4 and IPv6 listeners.
8. **Reconnect/disconnect:** Normal close, abrupt process kill, cable/Wi-Fi loss, host sleep, server restart and repeated scan. Verify each session releases socket/thread/FFmpeg/RTSP resources and old tokens/URLs stop working.
9. **Network loss:** Drop route during handshake, RTSP probe and streaming; verify deadlines and recovery without busy loops or stale active state.
10. **Multiple interfaces:** Run with Wi-Fi + Ethernet, VPN, Docker/Podman and libvirt bridges; select intended interface and verify listener binds only there. Change route/interface while app runs and confirm pairing refresh behavior.
11. **Duplicate connections:** Attempt two clients/duplicate Android connects; enforce documented policy (reject second or safely replace first) and verify no orphan session.
12. **FFmpeg failure:** Use unavailable ffmpeg, unsupported codec, RTSP timeout and child that ignores normal shutdown in a controlled harness; verify bounded wait and no orphan process.
13. **RTSP failure:** Server refuses, authenticates incorrectly, sends malformed stream, or disconnects after start; verify Linux cleanup and Android state reset.
14. **Resource exhaustion boundaries:** At local maximum connection count, descriptor length, parser count, concurrent stream and bitrate/resolution/FPS limits, verify rejection before resource allocation. Test timeout recovery with a slow sender.
15. **Windows legacy regression (if retained):** Cross-platform golden activation packet; truncated descriptor/error report; controlled parser sanitizers; verify route fallback and ADB cleanup.

## 20. Files Requiring Changes

For the active Linux/Android security path, likely implementation files are:

- `linux/src/main.cpp`, `network.cpp/.hpp`, `device_descriptor.cpp/.hpp`, `rtsp_probe.cpp/.hpp`, `ffmpeg_runner.cpp/.hpp`, `stream_options.cpp/.hpp`, `qr.cpp/.hpp`.
- `android/app/src/main/AndroidManifest.xml`, `android/app/build.gradle.kts`, `MainActivity.kt`, `QRScanner.kt`, `StreamActivity.kt`, `networking/ConnectionManager.kt`, `networking/DeviceDescriptor.kt`, `networking/connection/TCPConnection.kt`, `rtsp/StreamOptions.kt`, `rtsp/Streamer.kt`, and logging code.

If Windows support remains in scope, also `windows/src/net/server.cpp`, `connection.cpp`, `serializer.cpp`, `rtsp/receiver.cpp`, `rtsp/manager.cpp`, `rtsp/qrconview.cpp`, `adb.h`, plus `windows/VCamdroid.vcxproj` for dependency/protocol changes. Build files should change only if adding a vetted shared security/framing dependency or tests.

`M3_SECURITY_AUDIT.md` is the only file created by this investigation. No source/configuration was modified.

## 21. Final Milestone 3 Assessment

Milestone 3 security is **not complete**. The repository demonstrates an unauthenticated, cleartext control service; endpoint-only QR pairing; untrusted URL flow into a shell command; unbounded/unsafe parser behavior; and no application-level media credentials. Runtime confirms wildcard IPv4 binding on TCP/6969 and a successful active control/media path over Wi-Fi. Android RTSP listener scope and direct unauthenticated access remain unknown. Secure pairing, authenticated control, media-plane access control, safe parser framing/resource limits, and explicit interface binding should precede describing the system as securely paired.

### Historical runtime snapshot (superseded by current Section 22)

### Environment

- **OS:** Arch Linux, x86_64, kernel `7.2.7-arch1-1`.
- **Observed interfaces:** `lo` (`127.0.0.1/8`, `::1/128`); `wlo1` UP (`10.229.94.197/24`, global IPv6 `2402:3a80:4614:953a:d6:da37:d31f:fb47/64`, plus link-local IPv6); `virbr0` DOWN (`192.168.122.1/24`). Default IPv4 and IPv6 routes use `wlo1`. No Docker, Podman, VPN, or other active interface appeared in the observed interface list.
- **OpenCam:** PID 9707, command `./linux/build/opencam`, parent PID 9605, running in the foreground terminal.
- **FFmpeg:** PID 9716, parent PID 9707, command reads `rtsp://10.229.94.172:8554/live` and writes `/dev/video2`. No `ffprobe` process was present at the snapshot; the readiness probe is transient and had already completed for this active stream.
- **Android connection state:** No Android shell/process inspection was available. On Linux, an established control socket from `10.229.94.172:57174` and an established FFmpeg TCP connection to `10.229.94.172:8554` show that an Android-side peer is connected and the media path is active. The device identity itself was not independently verified.

### Listener Verification

Read-only `ss -ltnup` showed:

| Listener | Runtime observation |
|---|---|
| Linux control TCP/6969 | `0.0.0.0:6969`, LISTEN, PID 9707 (`opencam`), backlog shown as 4. This confirms the wildcard IPv4 bind at runtime. No `[::]:6969` listener was shown. |
| Linux UDP/6969 or UDP/8554 | No matching listener. |
| Linux TCP/8554 | No local listener. The RTSP server is on Android, not Linux. |
| Android TCP/8554 | The Linux FFmpeg client has an established TCP connection to `10.229.94.172:8554`; this confirms the endpoint is reachable from the control host during the active session, not its bind scope or access policy. |

The wildcard control socket is potentially reachable through every local IPv4 interface. In the observed state, `wlo1` is UP at `10.229.94.197`; `virbr0` is DOWN; loopback is local only. Firewall policy was not inspected or changed. Runtime `ss` also associated FFmpeg with the inherited OpenCam listening descriptor and established control descriptor (the same listener/control sockets appeared under both PIDs). Thus the child currently holds those descriptors while streaming; this is not evidence of an orphan, because OpenCam remains alive and is FFmpeg's parent.

### LAN Reachability

No extra `nc`/TCP probe was sent. The already-established Android control session is direct runtime evidence that a peer at `10.229.94.172` reached the listener through the host's Wi-Fi address `10.229.94.197:6969`; this is normal workflow, not a test of a second unauthorized device. The Android peer also reaches the advertised media endpoint from the Linux host. No second trusted LAN device was available to perform an independent connectivity check:

`RUNTIME VERIFICATION REQUIRED — SECOND LAN DEVICE UNAVAILABLE`

The live stream occupies the single synchronous Linux client handler. A no-data connection could block service handling and, if the active peer disconnected, become the next accepted client; it was not attempted. Descriptor timeout behavior is therefore not tested at runtime.

### RTSP Verification

The active FFmpeg command line uses `rtsp://10.229.94.172:8554/live`; `ss -tnp` showed a TCP connection from `10.229.94.197:34064` to that endpoint. This confirms RTSP endpoint reachability from Linux during the authorized-by-workflow session. It does **not** establish whether an unrelated LAN client can access it, whether authentication is required, or whether Android binds only that address or all interfaces.

No independent RTSP request was sent while the camera was active. Android-side socket inspection and access-policy inspection were unavailable, and no second LAN device was available. Result for direct access without control authorization: **UNKNOWN**. No video was requested, recorded, or redistributed by this verification.

### FFmpeg/ffprobe Verification

The active command resolves to `/usr/bin/ffmpeg`; `/usr/bin/ffprobe` is also installed. Both report FFmpeg `n9.0.2`. Their local `-protocols` output includes input protocols beyond RTSP, including `file`, `data`, `ftp`, `gopher`/`gophers`, `http`/`https`, `tcp`, `udp`, `unix`, `sftp`, `srt`, `zmq`, and additional protocols; output support also includes multiple non-RTSP protocols. This confirms the installed binaries have a broad protocol set, consistent with the static concern in M3-02.

No non-RTSP URL was supplied to OpenCam or FFmpeg. The protocol listing alone does not prove that this exact command line accepts each scheme, because `-rtsp_transport tcp` is also supplied. No SSRF or local-file access test was performed. M3-01's source path was rechecked: `linux/src/rtsp_probe.cpp:21-30` still concatenates the peer URL into a command and calls `std::system`. Exploit execution was intentionally not performed:

`CONFIRMED BY STATIC ANALYSIS — RUNTIME EXPLOIT EXECUTION NOT PERFORMED`

### Lifecycle Verification

At observation time, OpenCam PID 9707 and its FFmpeg child PID 9716 were both running; FFmpeg had an active control socket and active media socket. This confirms the current connected state, not cleanup behavior. No UI disconnect, process death, reconnect, network loss, FFmpeg failure, or RTSP failure was induced because that would disturb the user's active stream. No orphan process was observed at this snapshot, but orphan cleanup after disconnect remains untested. No `ffprobe` child remained at the snapshot.

The live socket table showed that FFmpeg retains inherited listener/control descriptors while its parent is alive. The source uses `fork`/`execlp` without closing those descriptors or setting close-on-exec. If OpenCam were to exit unexpectedly while FFmpeg survived, these descriptors could keep the listener/control sockets open; that failure case was not induced.

### Multi-interface Verification

The host currently has Wi-Fi `wlo1` UP and `virbr0` DOWN, with loopback. It has no second active interface, VPN, Docker, or Podman interface in the observed list. The active connection uses the Wi-Fi address `10.229.94.197`. Since only one non-loopback interface was active, the first-address selection policy under competing interfaces was not tested. QR terminal output was not captured directly; the active accepted socket's local address confirms the current Wi-Fi endpoint, but QR text itself is not independently observed.

`NOT TESTED — REQUIRED INTERFACE CONFIGURATION UNAVAILABLE`

ADB forwarding/listeners are **NOT APPLICABLE TO THE ACTIVE LINUX PATH**: Linux source has no OpenCam ADB forwarding implementation. The Windows-only ADB path was not running or inspected.

### Static vs Runtime Comparison

| Finding | Static conclusion | Runtime result | Final status |
|---|---|---|---|
| M3-01 shell injection | Peer RTSP URL is interpolated into `std::system` command. | Source rechecked; no injection payload was run, as required. | **CONFIRMED BY SOURCE; RUNTIME NOT REQUIRED** |
| M3-02 peer-controlled media endpoint/protocol | Unauthenticated descriptor URL reaches ffprobe/FFmpeg; no URL allowlist. | Active URL reached Android RTSP endpoint; installed ffmpeg/ffprobe support many protocols beyond RTSP. No alternate URL was attempted. | **CONFIRMED BY SOURCE AND RUNTIME** for installed protocol capability and active URL flow; arbitrary-scheme acceptance by this exact invocation remains **UNKNOWN**. |
| M3-03 descriptor memory/connection DoS | Unbounded accumulator and blocking single-client handler without timeout. | Not tested; active camera session made an idle probe unsafe. | **CONFIRMED BY SOURCE; RUNTIME NOT TESTED** |
| M3-04 Android unframed parser | Reader passes arbitrary chunks; Android handler ignores byte count and lacks validation. | No Android-side malformed/fragmented packet test; active stream left undisturbed. | **CONFIRMED BY SOURCE; RUNTIME NOT TESTED** |
| M3-05 Windows unsafe parser | Windows deserializers read without respecting received size. | Windows controller is not the active instance. | **CONFIRMED BY SOURCE; RUNTIME NOT TESTED** |
| M3-06 bind/interface selection | Linux binds IPv4 wildcard; address selection is first non-loopback IPv4. | `0.0.0.0:6969` confirmed; active interface `wlo1`; `virbr0` down; no IPv6 listener. | **CONFIRMED BY SOURCE AND RUNTIME** for wildcard bind and current interface set; competing-interface selection remains **NOT TESTED**. |
| M3-07 direct Android RTSP access | App configures no media credentials/TLS; library bind/default policy unknown. | Linux's active FFmpeg connection to phone `:8554` confirms paired-path reachability only. No independent client or Android socket inspection. | **UNKNOWN** for anonymous direct access and Android bind scope. |
| M3-08 Android EOF/reconnect lifecycle | Wi-Fi EOF handling, timeouts, replacement and cleanup are incomplete in source. | No disconnect/reconnect was induced. Active Linux child retained inherited descriptors. | **CONFIRMED BY SOURCE; RUNTIME NOT TESTED** for Android lifecycle; inherited Linux descriptor observation **CONFIRMED AT RUNTIME**. |
| M3-09 logging | Endpoint/URL/host details are logged by source. | Logs were not inspected during this runtime check. | **CONFIRMED BY SOURCE; RUNTIME NOT TESTED** |
| M3-10 Windows/Android activation mismatch | Serialization field widths/order differ. | No Windows activation was performed. | **CONFIRMED BY SOURCE; RUNTIME NOT TESTED** |

### Tests Skipped

- No TCP/6969 probe or idle connection: existing camera stream is active and Linux handles clients synchronously.
- No RTSP connection separate from FFmpeg and no second-device check: could add a competing session; second trusted test device was unavailable to the inspection environment.
- No disconnect/reconnect, camera stop, network loss, or child termination: these would change the active user's session.
- No malformed input, oversized message, fuzzing, shell payload, non-RTSP URL, or arbitrary destination attempt: prohibited by the safety scope.
- No interface changes, firewall changes, ADB mapping changes, or Windows runtime check.

### Historical interim assessment (superseded by current Section 23)

- **Now confirmed at runtime:** Linux OpenCam is running as PID 9707; it listens on `0.0.0.0:6969` over IPv4; the accepted control connection uses Wi-Fi address `10.229.94.197` with peer `10.229.94.172`; FFmpeg PID 9716 is its child and has an active TCP connection to the Android RTSP endpoint at `10.229.94.172:8554`; installed FFmpeg/ffprobe support numerous non-RTSP protocols; the FFmpeg child currently retains inherited listener/control descriptors.
- **Still confirmed by source only:** command injection in `waitForRtsp`; missing authentication/authorization; Linux unbounded descriptor handling; Android packet framing/validation and EOF defects; Windows unchecked parser and serializer mismatch; first-address interface selection behavior; Android RTSP server receives no app-configured credentials/TLS.
- **Still unknown:** whether Android RTSP binds wildcard or a single interface; whether an independent LAN client can access the active stream without control authorization; firewall reachability from other devices; whether this exact ffprobe/FFmpeg command accepts each supported non-RTSP protocol; behavior after OpenCam parent failure with inherited descriptors; actual QR text as displayed.
- **Not tested:** no-data timeout, malformed/oversized protocol handling, Android parser runtime failure modes, Windows runtime parsing, normal disconnect/reconnect cleanup, network loss, multiple active interfaces/VPN/container, and direct RTSP access from a second device.
- **Requires implementation:** remove shell command construction; add authentication and endpoint authorization; enforce protocol/URL allowlists; bound and frame control messages; harden Android and Windows parsers; protect RTSP media access; bind the control service to an intentional interface; close inherited descriptors and bound process shutdown.

Runtime evidence confirms that the current control listener is exposed on all local IPv4 interfaces and that the paired Wi-Fi media path is active. It does not establish anonymous direct RTSP access or safe lifecycle behavior. OpenCam is **not secure for untrusted networks**; the previously identified implementation work remains necessary.

## 22. Runtime Verification Results

### Environment

- **Host:** Arch Linux x86_64, kernel `7.2.7-arch1-1` (observed during this assessment).
- **Interfaces/routes:** `lo` (`127.0.0.1/8`, `::1/128`); `wlo1` UP (`10.229.94.197/24`, global IPv6 `2402:3a80:4614:953a:d6:da37:d31f:fb47/64`, plus link-local IPv6); `virbr0` DOWN (`192.168.122.1/24`). Default routes use `wlo1`. No active Docker, Podman, VPN, or second physical interface was observed. Firewall policy was not inspected or changed.
- **OpenCam baseline/current:** Linux process PID `9707`, executable `./linux/build/opencam`, parent `9605`. At the initial active-stream snapshot, FFmpeg PID `9716` was its child; its command was `ffmpeg -hide_banner -rtsp_transport tcp -i rtsp://10.229.94.172:8554/live -an -vf format=yuv420p -f v4l2 -pix_fmt yuv420p -video_size 640x480 /dev/video2`. After the stream ended and bounded tests completed, no FFmpeg/ffprobe child remained. The current listener remains `0.0.0.0:6969`, `LISTEN`, backlog 4, with no queued clients.
- **Android endpoint:** Peer observed as `10.229.94.172`. ADB is installed, but `adb devices -l` listed no attached device, so Android process/socket-table inspection was unavailable. The temporary ADB server started by that read-only query was stopped afterward; no mapping or configuration was changed.

### Listener Verification

`ss` showed Linux PID 9707 listening on `0.0.0.0:6969` over IPv4. No IPv6 `[::]:6969` listener appeared; loopback and the Wi-Fi IPv4 address both accepted TCP handshakes in the bounded checks. The wildcard bind means any local IPv4 interface that is up and permitted by host/network policy is a potential ingress path; during the observed state `wlo1` was up and `virbr0` was down. This confirms runtime binding, not that a remote second device can pass any external firewall.

During the active stream, `ss` showed the accepted control connection and Android media connection. The FFmpeg process shared the same listener and accepted-control socket inodes as OpenCam; `/proc/<ffmpeg-pid>/fdinfo` flags were `02` for descriptors 3 and 4, with no close-on-exec bit. FFmpeg also held its RTSP socket and `/dev/video2`. After the stream stopped, the child exited and only OpenCam's listener remained.

At one active-stream snapshot, `/proc` reported OpenCam RSS about 4.4 MiB with one thread; FFmpeg RSS about 75 MiB, virtual size about 1.02 GiB, 31 threads and FDSize 64. These are single-point observations, not exhaustion thresholds or a performance benchmark.

### Control Protocol, Authentication, and Resource Checks

All control tests were local to the OpenCam host and targeted `10.229.94.197:6969`; no QR was used by the test client.

- **Unauthenticated/incomplete:** A raw TCP connection succeeded without QR, token, or prior registration. Sending the 3-byte prefix `00 05 41` (string length 5, one body byte) did not cause a close during a 1.2-second observation. A second client's TCP handshake succeeded but appeared in the listener queue (`Recv-Q 1`, backlog 4); after client A closed, the queued client proceeded and the listener returned to `Recv-Q 0`. This dynamically confirms a single synchronous handler can be occupied by an incomplete descriptor and that the listener recovers on peer close. It does not measure a longer timeout; source shows none in this receive loop.
- **Bounded size/parser cases:** Separate clients sent incomplete string prefixes/payloads of 1 KiB, 4 KiB, 16 KiB, 64 KiB, and 256 KiB; an incomplete descriptor with a declared 65,535-item count; and a short descriptor with trailing data. No server response or prompt rejection occurred during the bounded 250 ms observation for each case. Clients then closed normally. OpenCam remained running and listening. These tests did not exhaust memory or establish behavior for longer-duration/unbounded input. No Android parser was involved.
- **Complete unauthenticated descriptor:** A 27-byte descriptor (one-byte invalid-UTF-8 name, `file:///dev/null` URL, zero resolution/filter counts) parsed without QR or credentials. Linux sent the 41-byte activation packet, demonstrating that the unauthenticated descriptor reached activation after local V4L2 discovery. A transient `ffprobe` PID (`11935`) was observed during the subsequent readiness attempts; the client closed normally, no FFmpeg stream child remained, and TCP/6969 continued listening. No shell payload was supplied.
- **Exposure from another device:** No second trusted LAN device was available to test. Host-to-host/local checks and the existing Android normal session do not substitute for an independent LAN reachability test.

### RTSP Verification

During the normal Android stream, the Linux host made separate RTSP requests to `10.229.94.172:8554/live`, without authorization headers or a test-client control session:

- `OPTIONS` returned `RTSP/1.0 200 OK` and advertised `DESCRIBE, SETUP, TEARDOWN, PLAY, PAUSE`.
- `DESCRIBE` returned `RTSP/1.0 200 OK`, SDP (`Content-Length: 439`), and `Content-Base: rtsp://:0/`.

This confirms the endpoint accepted unauthenticated RTSP capability/description requests from the Linux host while streaming. No `SETUP`/`PLAY` was sent and no video was retrieved or saved. This does not establish that another LAN device can retrieve media, whether the Android listener binds Wi-Fi/all interfaces/IPv6, or whether a remote firewall blocks access. After the observed stream/control session had ended, a new TCP connect from Linux to `10.229.94.172:8554` was refused. Since Android process state could not be inspected, the refusal cannot be attributed more specifically than “endpoint unavailable after the session ended.”

The normal session's active FFmpeg command had used the same Android `/live` URL over TCP, confirming ordinary RTSP operation. TLS was not used in the observed RTSP exchange. Source provides no RTSP authentication configuration.

### FFmpeg/ffprobe Verification

The host used `/usr/bin/ffmpeg` and `/usr/bin/ffprobe`, version `n9.0.2`. Their local protocol listing included non-RTSP handlers such as `file`, `data`, `ftp`, `gopher(s)`, `http(s)`, `tcp`, `udp`, `unix`, `sftp`, `srt`, and `zmq` (among others).

A standalone local ffprobe command using OpenCam's relevant RTSP options against `file:///dev/null` returned `Invalid data found when processing input` (exit 1). More importantly, the complete unauthenticated descriptor test caused OpenCam to spawn ffprobe for that same local URL. This confirms the probe path accepts a non-RTSP URL and starts the installed ffprobe; it does not demonstrate arbitrary file disclosure or remote SSRF. No external URL, unrelated local file, or other remote protocol was contacted. The exact same FFmpeg URL argument was observed working for the normal RTSP stream.

M3-01 remains **CONFIRMED BY STATIC ANALYSIS — RUNTIME EXPLOIT EXECUTION NOT PERFORMED**. `linux/src/rtsp_probe.cpp::waitForRtsp` still interpolates the peer URL into a quoted command passed to `std::system`; no shell-metacharacter payload or marker operation was run.

### Lifecycle Verification

The original active stream ended after the user was asked to disconnect normally, though the exact UI action/time was not independently observed. Its FFmpeg child disappeared and the Linux control listener returned to an empty `LISTEN` state. After bounded protocol tests, OpenCam remained alive, did not retain an FFmpeg/ffprobe child, and TCP/6969 remained available. This is limited evidence of cleanup on the observed session end and ordinary peer close; it is not a complete reconnect test.

No Android process termination, OpenCam shutdown, FFmpeg termination, RTSP failure, network loss, reconnect, duplicate Android connection, or parent-exit test was performed. During the active baseline, runtime FD inspection confirmed FFmpeg inherited the listening and accepted-control sockets without close-on-exec. The consequence of parent death while the child survives remains untested and is tracked in M3-11.

### Multi-interface Verification

Only `wlo1` was UP among non-loopback interfaces; `virbr0` was down. The observed Android peer/control route used `10.229.94.197` over Wi-Fi. Current wildcard binding was confirmed, but no active VPN, Ethernet, Docker, Podman, or libvirt bridge was available to test address-selection order or remote exposure over those paths. The QR was not captured directly during this turn; source still establishes that its payload is endpoint-only, and the observed accepted socket used the Wi-Fi address.

`NOT TESTED — REQUIRED INTERFACE CONFIGURATION UNAVAILABLE` for competing active interfaces. Android RTSP bind scope remains `UNKNOWN — LIBRARY BIND BEHAVIOR NOT OBSERVABLE FROM CURRENT TEST ENVIRONMENT`.

ADB forwarding/listeners are **NOT APPLICABLE TO THE ACTIVE LINUX PATH**. The ADB query found no attached device; no Windows controller or Windows ADB forwarding test was run.

### Static vs Runtime Comparison

| Finding | Static conclusion | Runtime result | Final status |
|---|---|---|---|
| M3-01 shell injection | Peer URL reaches `std::system` through shell-interpolated command. | Source path rechecked; no shell payload was executed. | **CONFIRMED BY SOURCE; RUNTIME NOT REQUIRED** |
| M3-02 unauthenticated URL/protocol control | No authentication or scheme/host allowlist; peer URL reaches ffprobe and FFmpeg. | No-QR descriptor reached activation and spawned ffprobe for `file:///dev/null`; installed protocol list is broad. Ordinary RTSP works. No arbitrary remote protocol tested. | **PARTIALLY CONFIRMED**; local non-RTSP probe path confirmed, broader SSRF/destination behavior remains UNKNOWN. |
| M3-03 descriptor memory/connection DoS | Unbounded accumulator, swallowed parser errors, synchronous single-client receive without deadline. | Partial descriptor held handler ≥1.2 s; client B queued until A closed. Incomplete inputs through 256 KiB and a huge count did not crash, but were not promptly rejected. | **CONFIRMED BY SOURCE AND RUNTIME** for starvation/no prompt rejection; exhaustion magnitude not tested. |
| M3-04 Android parser framing/validation | TCP read chunks treated as commands; supplied byte count ignored. | No Android device attached to ADB; no Android parser packet test. | **CONFIRMED BY SOURCE; RUNTIME NOT TESTED** |
| M3-05 Windows unsafe parser | Windows deserializers ignore received-size bounds. | Windows controller not active. | **CONFIRMED BY SOURCE; RUNTIME NOT TESTED** |
| M3-06 wildcard bind/address selection | Linux binds IPv4 wildcard; advertised address is first non-loopback IPv4 from `getifaddrs`. | `0.0.0.0:6969` confirmed; wlo1 up, virbr0 down, no IPv6 listener. Multi-interface ordering unavailable. | **CONFIRMED BY SOURCE AND RUNTIME** for current wildcard bind; selection under multiple active interfaces NOT TESTED. |
| M3-07 Android RTSP access | No app-configured credentials/TLS; library scope unknown. | Unauthenticated OPTIONS and DESCRIBE each returned 200 from Linux while active; later TCP connection refused after session ended. No media retrieval or second-device test. | **PARTIALLY CONFIRMED**; anonymous protocol metadata access confirmed, media access and bind scope UNKNOWN. |
| M3-08 Android EOF/connect lifecycle | EOF, timeout, and replacement cleanup weaknesses in Android source. | No Android-side parser/lifecycle test or reconnect. Linux stream child exited after the observed session ended. | **CONFIRMED BY SOURCE; RUNTIME NOT TESTED** for Android behavior. |
| M3-09 endpoint/URL logging | Sources log URLs and endpoint details. | No app logs collected during active tests. | **CONFIRMED BY SOURCE; RUNTIME NOT TESTED** |
| M3-10 Windows activation mismatch | Windows field widths/order differ from Android decoder. | No Windows activation test. | **CONFIRMED BY SOURCE; RUNTIME NOT TESTED** |
| M3-11 inherited descriptors | Fork/exec does not close listener/control descriptors or set close-on-exec. | Same socket inodes observed in OpenCam and FFmpeg; fd flags lacked CLOEXEC. Parent-exit case not induced. | **PARTIALLY CONFIRMED**; inheritance confirmed at runtime, orphan/restart impact remains LIKELY. |

### Tests Skipped or Limited

- No second trusted LAN device was available; remote firewall/reachability and independent media access remain untested.
- No Android ADB device was attached; Android bind address, malformed activation handling, EOF behavior, and process lifecycle remain untested.
- No media `SETUP`/`PLAY`, stream capture, shell-injection proof, external URL, unrelated file, or arbitrary-protocol destination was attempted.
- No 1 MiB payload, sustained slow sender, repeated high-rate connection test, or resource-exhaustion test was run. Bounded payloads were limited to 256 KiB to avoid unnecessary CPU/memory load; no RSS growth conclusion is claimed.
- Invalid dimension values, broad unexpected field-order variants, and Android fragmented/coalesced/truncated activation packets were not exercised. The tested malformed cases are enumerated above; no general parser robustness claim is made.
- No forced process termination, reconnect cycle, network loss, duplicate session, interface change, or parent-exit test was run. Only the observed stream end and ordinary test-client closes were observed.
- No Windows runtime validation or aggressive fuzzing was performed.

## 23. Updated Milestone 3 Assessment

- **Confirmed dynamically:** Linux OpenCam PID 9707 listens on IPv4 wildcard TCP/6969; unauthenticated no-QR clients can connect; an incomplete descriptor blocks the synchronous handler while another client queues; a complete unauthenticated descriptor reaches a 41-byte activation packet and starts ffprobe for `file:///dev/null`; the process recovers after peer closure and remains listening. During active streaming, unauthenticated RTSP OPTIONS/DESCRIBE from the Linux host returned 200 with SDP. FFmpeg inherited OpenCam's listening and accepted-control socket descriptors without close-on-exec. A normal RTSP stream had worked before the session ended.
- **Still source-confirmed without runtime exploit/test:** M3-01 shell injection (payload intentionally not executed); Android parser framing/EOF defects; Windows unsafe deserialization and packet mismatch; endpoint-only QR/no cryptographic identity; absence of control authentication and URL policy; logging details.
- **Partly confirmed / still unknown:** M3-02's broader non-RTSP/network access impact; M3-07 anonymous media retrieval and Android bind scope; M3-11 behavior after parent exit. Remote LAN reachability from a second device and firewall policy remain unknown. Multi-interface address selection was not tested with multiple UP interfaces.
- **Implementation remains necessary:** Remove shell-mediated ffprobe invocation; authenticate and authorize both peers; constrain URL schemes/destinations; frame and cap descriptors/control messages; add deadlines and concurrency/resource limits; protect RTSP media independently; choose/bind intended interfaces; close inherited descriptors and bound child shutdown; harden Android and Windows parsers if those clients remain supported.

The runtime checks strengthen the unauthenticated control-plane and anonymous RTSP metadata findings, but they do not prove third-party LAN media retrieval, broad SSRF, or exploit impact beyond the tested cases. OpenCam is **not secure for untrusted networks**. Further isolated testing is required for Android parser behavior, independent RTSP media access, process-failure cleanup, and multi-interface exposure.

## 24. Security Remediation Results

**Remediation date:** 2026-09-27. The source changes below supersede the affected “current behavior” statements in Sections 1–23; those sections are retained as historical audit/runtime evidence and are not silently rewritten. This was a source/build/unit-test remediation pass. No Android device was attached for end-to-end verification, and no Windows build host was available.

### Changes implemented

- **Linux control protocol and trust:** Added protocol v1 frames (`OCAM`, version, type, bounded 32-bit length), a 64 KiB message limit, five-second handshake and thirty-second descriptor deadlines, strict descriptor parsing, and authenticated sequenced control frames. QR pairing now carries a 256-bit random secret, expires after ten minutes, and rotates after a successful pairing. Mutual HMAC-SHA256 proofs authenticate the client and Linux server; a fresh session key authenticates later frames and derives a separate media password. Pairing secrets are not emitted in logs. Authentication provides no confidentiality: control traffic remains plaintext on the LAN.
- **Endpoint policy:** Linux accepts only an IPv4 RTSP URL targeting the authenticated TCP peer on port 8554 at `/live/<random 128-bit path>`. Userinfo, query/fragment, alternate schemes, hosts, ports, and paths are rejected before V4L2/probe/process setup. FFprobe and FFmpeg both use an RTSP/TCP protocol allowlist.
- **Shell/process handling:** `linux/src/rtsp_probe.cpp::runProbe` now uses `fork`/`execlp` with the URL as a single argv element, a bounded wait, termination escalation, and child reaping. `linux/src/ffmpeg_runner.cpp::startFfmpeg` closes descriptors from 3 upward before exec; stream shutdown sends SIGTERM, waits, escalates, and reaps. The Linux listener and accepted sockets are created close-on-exec as defense in depth. The only remaining Linux `std::system` call is a fixed `modprobe` command with no peer-controlled interpolation.
- **Network exposure:** Linux now chooses an IPv4 address using kernel route selection, falls back to an UP non-loopback IPv4 address, and binds TCP/6969 to that specific address rather than `0.0.0.0`. The implementation is intentionally IPv4-only. Route selection can choose a VPN/default-route interface; no explicit interface-selection UI was added.
- **Android:** Wi-Fi pairing and control now use the framed/authenticated protocol with bounded reads, exact byte-count handling, handshake/activation timeouts, message ordering checks, EOF termination, and connection replacement/cleanup. The QR parser requires the new token format. Descriptor and activation parsing validate lengths, UTF-8, enums, dimensions, FPS, bitrates, focus, booleans, filters, and trailing bytes. RTSP uses a random per-stream path and session-derived password for Wi-Fi sessions; credentials are rotated after the server stops. Library logging is disabled.
- **RTSP limitation:** The inspected Android RTSP dependency (RTSP Server 1.3.6) exposes per-server Basic authorization and IPv4 mode, but its auth check protects DESCRIBE and not all RTSP methods; its public server API offers no bind-address parameter and binds wildcard IPv4. Basic authorization is plaintext on this non-TLS RTSP connection. Random paths and credentials reduce casual/off-path access but do not provide complete media-plane authorization or confidentiality. Direct RTSP security is therefore **PARTIALLY FIXED**, not solved.
- **Windows legacy path:** The legacy controller is now explicitly documented as ADB-only and binds its control server to `127.0.0.1`; its unauthenticated protocol is no longer offered on LAN. The ADB descriptor now has a bounded 4-byte length frame. Deserialization is bounds/count/UTF-8/range checked; activation fields now align with Android, including one-byte booleans, focus, flash, filter encoding, and canonical bit/s bitrate values (the Windows UI's kbit/s values are scaled on serialization). Windows QR/Wi-Fi support is intentionally unavailable until the authenticated protocol is ported. The Windows project was not compiled in this environment.
- **Logging:** Explicit network-controlled URLs, device names, error text, and media credentials were removed from Linux connection/probe logging. Android connection failures and reader errors use generic messages; RTSP library logs are disabled. Windows displays/logs redacted descriptor URLs and generic malformed-input errors.
- **Regression tests:** Added Linux CTest coverage for mutual proof/session frame authentication, invalid UTF-8/count rejection, RTSP endpoint allowlisting, and harmless shell-metacharacter handling. Added Android unit tests for token validation, fragmented handshake, authenticated frame interoperability, and wrong-secret rejection.

### M3-01 through M3-11 status

| Finding | Status | Evidence / remaining limitation |
|---|---|---|
| M3-01 shell injection | **FIXED** | Shell construction removed; argv-based `execlp`; CTest marker regression passes. No shell payload is executed by the regression test. |
| M3-02 unauthenticated endpoint/protocol control | **PARTIALLY FIXED** | QR-secret mutual authentication, sequenced HMAC control frames, peer/port/path allowlist, and FFmpeg protocol whitelist implemented. No TLS/confidentiality; a captured QR secret can be used until its one successful pairing/expiry, and control metadata is observable. End-to-end pairing was not tested on a phone. |
| M3-03 unbounded descriptor/handler DoS | **PARTIALLY FIXED** | Bounded framing/parser and finite handshake/descriptor deadlines remove unbounded accumulation and indefinite idle occupancy. The single Linux handler remains serial, so an authenticated peer can delay later clients up to the configured descriptor timeout; no concurrency stress test was run. |
| M3-04 Android stream parser | **PARTIALLY FIXED** | Wi-Fi uses frame parsing; legacy ADB stream commands have bounded fragmentation/coalescing parser and EOF cleanup. Android unit tests/build pass, but malformed activation/real-device lifecycle was not exercised on-device. |
| M3-05 Windows parser | **PARTIALLY FIXED** | Descriptor length frame and bounded parser implemented; legacy error reports remain bounded, length-parsed stream messages. Windows is loopback/ADB-only and was not compiled or runtime-tested here. |
| M3-06 wildcard/interface selection | **PARTIALLY FIXED** | Linux uses route-selected IPv4 and binds that specific address; IPv6 intentionally unsupported, and VPN/default-route or multi-interface behavior is not runtime-tested. Windows is loopback-only. |
| M3-07 RTSP media access | **PARTIALLY FIXED** | Random path and per-session Basic credentials configured; Linux restricts destination and derives media credentials. Library fails to authorize all methods, binds wildcard IPv4, and provides no TLS; sniffing/direct-method access remains a risk. |
| M3-08 Android connection lifecycle | **PARTIALLY FIXED** | Connect/handshake/activation deadlines, EOF handling, replacement close, generation checks, and idempotent cleanup implemented. Real Android disconnect/reconnect/process-death behavior remains unverified. |
| M3-09 sensitive logging | **PARTIALLY FIXED** | Application-controlled URL/credential/error logging removed or redacted in Linux, Android, and Windows paths inspected. No device/runtime log capture was available; third-party logging behavior outside configured library flags remains unverified. |
| M3-10 Windows activation mismatch | **PARTIALLY FIXED** | Windows and Android schemas now agree on widths/order/booleans/focus/flash/filter fields in source. Windows compile/golden cross-language serialization test and runtime validation remain unavailable. |
| M3-11 inherited descriptors/child cleanup | **PARTIALLY FIXED** | Probe/FFmpeg child closes unrelated descriptors before exec; Linux sockets are CLOEXEC; FFmpeg has bounded terminate/escalate/reap. Runtime `/proc` inheritance and parent-exit cleanup were not tested after changes. |

“Partially fixed” includes residual architecture/API limitations or missing platform/runtime verification; it does not mean the corresponding source change was omitted.

### Build and tests performed

- Linux configure/build: `cmake -S linux -B linux/build && cmake --build linux/build -j2` — **PASS** after adding the missing `<chrono>` include found by the clean remediation build.
- Linux tests: `ctest --test-dir linux/build --output-on-failure` — **PASS, 2/2** (`opencam_security_tests`; `opencam_shell_injection_regression`). The shell regression confirms its harmless command-substitution marker is not created.
- Android: `cd android && ./gradlew testDebugUnitTest assembleDebug` — **PASS**, including unit tests and APK assembly after the ADB descriptor-length framing adjustment.
- Windows: **NOT BUILT**; Visual Studio/MSBuild is unavailable in the Linux environment.
- Hardware/integration: Android-to-Linux QR pairing, camera activation, RTSP authorization, V4L2 output, disconnect/reconnect, and stale-credential behavior were **NOT VERIFIED** because no Android device was attached. Do not interpret successful compilation/unit tests as proof that the complete camera pipeline still works.

### Compatibility and migration

Linux/Android Wi-Fi control now requires the new `OCAM1|IPv4|6969|<64-hex-secret>` QR and framed protocol v1; old Android clients and endpoint-only QR payloads are incompatible and must be updated together. The legacy Windows controller has been intentionally narrowed to authorized ADB/USB operation; Android now length-frames its ADB descriptor, so both sides must use updated builds. Existing LAN/Wi-Fi Windows usage is not supported by this remediation. No dependency was added to Android; Linux now requires OpenSSL Crypto (`find_package(OpenSSL REQUIRED)`).

## 25. Post-Remediation Assessment

The main confirmed Linux shell-injection sink is removed and covered by a harmless regression test. Linux control messages now require short-lived QR-secret authentication and per-session integrity checks before a descriptor can reach V4L2, probing, or FFmpeg; parser/resource bounds, URL restrictions, route-specific binding, and child descriptor cleanup have been added. Android and Windows parsers have bounded schemas and lifecycle/error handling changes. These changes materially improve the prior trust model, but **OpenCam is not yet secure for hostile networks**: control and RTSP traffic are not encrypted; RTSP library authorization does not gate every method and cannot bind to a specific interface; Windows and multi-interface changes lack build/runtime validation; and the full Android → RTSP → Linux → V4L2 pipeline has not been retested on hardware. A TLS-protected or authenticated media tunnel and complete RTSP method authorization remain future security work.

## 26. Real-Device Integration / Connection Failure Resolution

**Verification date:** 2026-09-28. This section records the physical-device integration pass and supersedes the “no Android device attached” limitation in Section 24. Historical pre-remediation evidence in Sections 1–23 remains unchanged.

### Environment and transport selection

- One authorized Android device was available over USB/ADB. Its identifying model/serial is intentionally omitted here.
- Linux and Android were on the same Wi-Fi subnet for the successful control-plane attempts. Linux used `10.229.94.197`; the Android peer appeared as `10.229.94.172`.
- The Android app displayed Wi-Fi as selected while the USB cable remained attached. The Wi-Fi QR path was used; it did not silently switch to USB/ADB.
- The Linux listener was route-bound to `10.229.94.197:6969` over IPv4. The runtime Linux OpenCam executable was rebuilt from the current worktree.

### Root causes and fixes observed

1. **Original no-connect failure:** the older Android startup logic preferred USB merely because a cable was attached, then tried `127.0.0.1:6969`; there was no ADB reverse mapping and no Linux listener on that loopback endpoint. A separate earlier attempt also had Linux and Android on different subnets. The explicit persisted Wi-Fi/USB selector removed cable-presence auto-selection; Wi-Fi now always takes the authenticated QR path. On the test subnet the phone completed the Wi-Fi connection with USB still attached.
2. **Authenticated descriptor deadlock:** `AuthenticatedChannel.receive()` and `send()` held the same monitor. The reader blocked waiting for activation while the descriptor sender waited for that monitor; Linux consequently hit its 30-second descriptor timeout. Separate send/receive locks plus `blockedReceiveDoesNotBlockFullDuplexDescriptorSend` fixed and regression-tested this. Runtime logs then showed the descriptor being sent immediately and Linux accepting it.
3. **RTSP probe deadline:** Linux's `ffprobe` child had a four-second wall-clock deadline, shorter than FFprobe's default five-second stream-analysis window for this camera. It exited with status 1 or hit the parent deadline even though an equivalent bounded manual probe read H.264/AAC SDP and H.264 packets successfully. The readiness probe now requests a one-second analysis window and has an eight-second hard deadline. In the final run, Linux reported `RTSP server is ready`.
4. **Credential encoding/guard consistency:** session-derived RTSP credentials now use 64 lowercase hex characters on both platforms, with a deterministic matching Android/Linux test vector. During this change, a stale Linux guard still required the old 43-character Base64 length and closed the session before activation; this guard was updated to 64. The final run passed descriptor validation, activation, and RTSP readiness. No pairing or authentication bypass was introduced.

### End-to-end stage results

| Stage | Runtime result |
|---|---|
| Wi-Fi mode with USB attached | **Confirmed.** The UI had Wi-Fi selected; the accepted TCP peer was Android over Wi-Fi. |
| QR parsing / pairing secret | **Confirmed operationally.** The fresh OCAM1 QR completed the authenticated handshake. Old/rotated QR scans were rejected, as expected. The secret was not recorded in this report. |
| Mutual control authentication | **Confirmed.** Android logged “Authenticated control connection established”; Linux accepted the authenticated descriptor. |
| Descriptor transmission | **Confirmed.** Android logged “Device descriptor sent”; Linux logged “Authenticated descriptor accepted”. The full-duplex lock regression test passes. |
| Activation | **Confirmed.** Linux logged “Activation sent”; Android started its stream on the successful activation runs. |
| Android camera/RTSP startup | **Confirmed.** Android logged “Stream started”; the Linux readiness check later succeeded. A manual `ffprobe` against the active local endpoint returned status 0 and identified H.264 at 640×480 plus AAC audio. No footage was saved. |
| Linux FFmpeg/V4L2 output | **Not successful / unresolved.** Linux logged that FFmpeg started, then observed child exit status **8**. The process was gone before the next inspection. `v4l2-ctl --device=/dev/video2 --get-fmt-video --get-parm` then returned `VIDIOC_G_FMT: Invalid argument` and `VIDIOC_G_PARM: Invalid argument`; no frames or configured output format were verified. FFmpeg stderr is intentionally suppressed, so exit code 8 alone does not establish whether the cause is V4L2 setup, muxer configuration, or another FFmpeg error. The user requested stopping further hardware retries at this point. |
| USB/ADB-selected mode | **Not tested end-to-end.** The available Linux controller implements authenticated Wi-Fi; the Android USB choice remains the explicitly labeled legacy localhost/ADB path and was not connected to a Linux ADB forward. |
| Disconnect/reconnect | **Partially observed.** When Linux closed a failed readiness/FFmpeg session, Android logged control disconnect, stopped the streamer, and returned to the app. A successful full-pipeline reconnect was not performed. |

### Changes and verification

- Added a visible persisted `ConnectionMode.WIFI` / `ConnectionMode.USB` selector. USB presence no longer chooses the transport automatically; Wi-Fi always uses the authenticated OCAM1 QR workflow.
- Fixed full-duplex synchronization in Android's authenticated channel and added a regression test that starts a blocking receive while sending a descriptor.
- Standardized the session-derived media password to lowercase hex and added identical fixed-vector checks in Android and Linux tests. Updated Linux's expected credential length.
- Reduced FFprobe analysis work for readiness and increased its bounded child deadline to eight seconds. Added sanitized numeric FFprobe/FFmpeg exit diagnostics; URLs and credentials remain unlogged.
- Linux build and CTest: **PASS, 2/2** after the final Linux edits. The socketpair test was run outside the restrictive syscall sandbox because the sandbox rejects its local `send()` call.
- Android `testDebugUnitTest assembleDebug`: **PASS** for the installed debug build and the full-duplex/media-credential tests.
- The phone received the rebuilt APK and was launched. The final active session reached FFmpeg startup, but it exited with status 8 before V4L2 frames could be demonstrated.

### Updated integration assessment

The original connection failure is resolved: explicit Wi-Fi mode works with USB physically attached, pairing authenticates, the descriptor/activation exchange completes, and RTSP readiness succeeds. This is meaningful real-device integration progress, but the central webcam result remains **INCOMPLETE** because the OpenCam FFmpeg child exits with status 8 and `/dev/video2` output frames were not confirmed. Do not describe the full Android → RTSP → Linux FFmpeg → V4L2 pipeline as working until FFmpeg's exit is diagnosed and frames are observed. The exit reason, USB-selected mode, and reconnect after successful camera output remain unresolved.
