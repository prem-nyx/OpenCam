# VCamdroid - Windows server

## Support status

The Windows server is a legacy ADB-only controller. Its unauthenticated legacy
protocol is bound to `127.0.0.1` and is intended to be reached only through an
authorized ADB reverse tunnel; do not expose or forward port 6969 to a LAN.
The versioned, QR-secret-authenticated Wi-Fi protocol is currently implemented
by the Linux controller and Android client, not by this Windows server. As a
result, Windows QR/Wi-Fi pairing is intentionally unsupported until that
protocol is ported. The ADB descriptor uses a bounded 4-byte length frame.

## Requirments & Dependencies

- Visual Studio 2022 with Dekstop C++ development package installed
- vcpkg with the following dependencies installed:
    - **ASSO**: ```vcpkg install asio```
    - **wxWidgets**: ```vcpkg install wxwidgets```

## Build

Before building VCamdroid [softcam](https://github.com/tshino/softcam) needs to be built.

### Softcam library

Open ```3rdparty/softcam/softcam.sln``` and build the solution in ```Release x64``` configuration.

Next open ```3rdpart/softcam/examples/softcam_installer.sln``` and build the solution in ```Release x64``` configuration.

For more information about the building process of softcam see [this](https://github.com/tshino/softcam?tab=readme-ov-file#how-to-build-the-library).

### VCamdroid

Open the ```VCamdroid.sln``` and build the solution in ```Release x64``` configuration. All required files will be placed in the ```dist``` directory. 

Now from the root directory you can run ```install.bat``` to install the DirectShow filter (softcam.dll)
