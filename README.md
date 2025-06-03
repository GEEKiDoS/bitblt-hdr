# BitBlt HDR
Fixes overexposed hdr screenshot for softwares that using Bitblt api

Using DXGI Desktop Duplication API and DX11 Compute Shader, It requires GPU that supports DX11 to works properly

### Download
Go to [releases](https://github.com/GEEKiDoS/bitblt-hdr/releases)

### How to use
1. Close your screenshotter
2. Put `version.dll` next to the exe of your screenshotter
3. You are good to go

### Tested Screenshotters
1. Tencent QQ (9.9.12-26466, NT Build with screenshot code in `wrapper.node`)
    - In some version of Windows, You need to place it to `versions\<lastest version folder>` to get it working #6
2. Tencent QQ (9.7.23, old non-NT 32bit build)
    - In order to get it working, "Override high DPI scaling behavior" must set to "Application" in Compatibility setting for QQ.exe
3. Snipaste (2.10.6)
4. Flameshot (12.1.0)

### Known Issue
1. ~~Multi-monitor is broken~~ Fixed in 0.6
2. ~~Screenshot under 1680x1050 is broken~~ Fixed in 0.8

### Demo Video (with Snipaste)
https://github.com/user-attachments/assets/0ef47c17-b880-4819-943f-74253cb13bf0

### Disclaimer
Use it with QQNT could somehow resulted in a banned account.
