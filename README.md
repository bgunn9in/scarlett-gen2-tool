# Scarlett Gen 2 Tool

[Русская версия](README.ru.md)

Read the firmware version, toggle Low Power Mode, and switch between firmware builds 1051 and 1116 on a Focusrite Scarlett 2i2 2nd Gen (USB ID `1235:8202`).

## Install or run from the ZIP

Use Windows 10 or 11 x64 with Focusrite driver 4.150.0.432. The app checks the SHA-256 hashes of two installed driver files. A different build can cause device detection to fail. Connect one interface at a time. [Focusrite model downloads](https://downloads.focusrite.com/focusrite/scarlett-2nd-gen/scarlett-2i2-2nd-gen) · [Driver 4.150.0.432 installer](https://fael-downloads-prod.focusrite.com/customer/prod/downloads/focusritedriver_4.150.0_432.exe)

Extract the [v1.2.0 ZIP](https://github.com/bgunn9in/scarlett-gen2-tool/releases/download/v1.2.0/scarlett-gen2-tool-v1.2.0-windows-x64.zip) and choose how to run the app.

- Run `Scarlett-Gen2-Tool-Setup.exe` to install the app and both firmware images for your Windows account. Setup adds a Start menu shortcut.
- For the portable version, run `Portable\scarlett-gen2.exe`. Keep `8202-1051.bin` and `8202-1116.bin` beside the EXE. The app looks for them in its own folder regardless of the working directory.

Compare the ZIP's SHA-256 hash with `SHA256SUMS.txt` from the same release. [Release files](https://github.com/bgunn9in/scarlett-gen2-tool/releases/tag/v1.2.0)

```powershell
Get-FileHash .\scarlett-gen2-tool-v1.2.0-windows-x64.zip -Algorithm SHA256
Get-Content .\SHA256SUMS.txt
```

## Read status or change a setting

The app starts in English. Choose `Русский` from the `Language` list in the top right to switch to Russian. The app remembers your choice.

The app reads the firmware version from the USB descriptor and Low Power Mode from `App_Env`. It refreshes both when you reconnect the interface. Click `Refresh status` to read them again.

- Click `Enable` or `Disable` under Low Power Mode. The mode limits current to the phantom power circuit. The app recognizes two `PhantomPower` values and shows an error if it cannot read the setting.
- To change firmware, select 1051 or 1116, click `Flash…`, and confirm the version change. Keep USB connected until the operation finishes. The app checks the current firmware and the selected image's hash, then reads back the entire written segment before restarting the interface.

### Backups and failed writes

Before erasing a segment, the app saves a full copy under `%LOCALAPPDATA%\FocusriteScarlettGen2\Backups`. The operation status shows the folder containing `App_Upgrade-<version>.bin` for a firmware change or `App_Env.bin` for a Low Power Mode change, along with `update-state.txt`. If `App_Env` contains additional data, the app stops before erasing it.

If the app verifies the write and asks you to reconnect the interface, reconnect it once and check the firmware version and mode. If erasing, writing, or readback fails, keep the backup and log and check whether Windows still detects the interface before trying another write. The backup covers one segment. The app has no command to restore it or recover an undetected device.

## Firmware images

The files in `firmware/` are original Focusrite images for PID `8202`. The app writes the selected image to `App_Upgrade` through the Focusrite driver's PAL interface.

| File | Source and offset in the extracted executable | SHA-256 |
| --- | --- | --- |
| [`8202-1051.bin`](firmware/8202-1051.bin) | Windows updater 1.0.1.165, PE `Scarlett (2nd Gen) Firmware Updater.exe`, offset `0x183410`, 64,000 bytes | `f8084934586ad894d5c740d4ac6d7232b00dd48da47f12f1c8206477261f021f` |
| [`8202-1116.bin`](firmware/8202-1116.bin) | macOS updater 1.0.1.178, Mach-O `ScarlettUpdater`, resource `Scarlett_2i2S_bin`, offset `0x116420`, 64,256 bytes | `49782a53d27664c5d58494e69d347410e7996f2af1687bb3ea8e2820ab720c98` |

To extract the images yourself, give `prepare-firmware.ps1` the paths to innoextract and 7-Zip. The script downloads the original packages, checks their hashes, and verifies the extracted images. Extraction does not run the installers. [Focusrite Windows package 1.0.1.165](https://fael-downloads-prod.focusrite.com/customer/prod/s3fs-public/focusrite/downloads/31626/scarlett-2nd-gen-firmware-updater-1.0.1.165.exe) · [Focusrite macOS package 1.0.1.178](https://fael-downloads-prod.focusrite.com/customer/prod/s3fs-public/focusrite/downloads/32269/scarlettupdater-1.0.1.178.dmg) · [innoextract](https://github.com/UserUnknownFactor/innoextract_win/releases/tag/670) · [7-Zip](https://www.7-zip.org/)

```powershell
.\prepare-firmware.ps1 -Innoextract 'C:\Tools\innoextract.exe' -SevenZip 'C:\Program Files\7-Zip\7z.exe'
```

The script saves the images to `build\`. To put them beside an existing EXE, pass its folder as `-OutputDirectory`.

## Build and test

Open Developer PowerShell for VS with the x64 MSVC tools and Windows SDK installed. The build uses the images in `firmware/`.

```powershell
.\build.ps1
.\build\scarlett1116.exe --self-test-full .\build\8202-1051.bin .\build\8202-1116.bin
```

The build produces `build\scarlett-gen2.exe` with both images beside it and `build\scarlett1116.exe` for testing and diagnostics.

To build the ZIP and installer, pass the path to the Inno Setup compiler. The ZIP and checksum file go to `build\release-v1.2.0\`. [Inno Setup](https://jrsoftware.org/inno-setup/downloads)

```powershell
.\package.ps1 -InnoCompiler 'C:\Program Files (x86)\Inno Setup 6\ISCC.exe' -Version 1.2.0
```

On Windows 11, the GUI detected a connected Scarlett and displayed firmware 1116 (`0x045C`) with Low Power Mode off. A separate run of the command-line updater moved the physical unit from 1051 to 1116. The transport tests simulated upgrades, downgrades, both mode changes, and failed or rejected writes.
