# lbp810_capt

`lbp810_capt` makes the Canon LBP-810 print from ordinary Windows 11 x64 applications. The printer has no page description language of its own: it only understands Canon's CAPT protocol and a pre-rendered, compressed bitmap, so every page has to be rasterized and encoded on the PC.

The project is intentionally narrow: one printer model, A4 paper, USB. It is a working bridge, not a WHQL-signed production driver, and it is not affiliated with Canon.

Русская версия: [README_RU.md](README_RU.md).

## How it works

```text
application ──► Windows v4 print queue ──► PWG Raster file ──► lbp810_spool.exe ──► USBPRINT ──► LBP-810
                (Microsoft PWG Raster      (%ProgramData%\     (decode, threshold,   (CAPT +
                 class driver + PDC)        CanonLBP810\Jobs)   scale to 600 dpi)     SCoA)
```

1. **Driver package.** `driver-package/` is a v4 printer driver built on the in-box *Microsoft PWG Raster Class Driver*. Windows renders every job to PWG Raster and writes it to a local file port.
2. **Bridge.** `lbp810_spool.exe --watch-capture` runs as a `SYSTEM` scheduled task. It atomically claims each finished job file, validates every page, decodes the PWG PackBits rows and thresholds them into a 1-bit A4 raster at 600 dpi (300-dpi input is doubled).
3. **CAPT.** Pages are compressed with SCoA and sent over the USBPRINT device interface using [libcapt](https://github.com/darkvision77/libcapt). Multi-page jobs are one CAPT job; a page the engine rejects after a jam or paper-out is re-sent.
4. **Waiting.** If the printer is out of paper, the cover is open or it is warming up, the bridge waits up to 30 minutes and logs the reason. A disconnected printer is retried for 10 minutes. Failed jobs are kept as `*.failed.pwg`.

Everything the bridge does is written with timestamps to `%ProgramData%\CanonLBP810\Jobs\bridge.log`:

```text
2026-10-24 21:15:09 Processing from-windows-app-7718937.processing.pwg.
2026-10-24 21:15:09 PWG job has 3 valid page(s).
2026-10-24 21:15:09 Sending page 1.
2026-10-24 21:15:09 Sending page 2.
2026-10-24 21:15:23 Sending page 3.
2026-10-24 21:15:43 Printer reports 3 page(s) finished.
2026-10-24 21:15:43 PWG job converted and printed successfully.
```

## Requirements

- Windows 11 x64
- Visual Studio 2026 with *Desktop development with C++*
- Windows SDK and WDK 10.0.28000 (`signtool.exe`, `Inf2Cat.exe`)
- CMake 3.21+, internet access on first configure (libcapt is fetched at a pinned commit)

## Build

From an x64 *Developer PowerShell for Visual Studio* in the repository root:

```powershell
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=cl.exe
cmake --build build-release
```

The executable is written to `build-release\lbp810_spool.exe`.

## Install

From an elevated PowerShell:

```powershell
.\tools\Install-CanonLBP810TestQueue.ps1
```

The script regenerates and signs the driver catalog, installs or updates the driver, copies the bridge to `%ProgramFiles%\CanonLBP810`, creates the `Canon LBP-810 CAPT App Queue` queue and starts the `CanonLBP810 CAPT Bridge` task. Then select that queue in any print dialog.

| Option | Description |
|---|---|
| *(none)* | Install, or update an existing installation in place. |
| `-RecreateQueue` | Delete and re-add the queue so Windows drops cached capabilities and defaults. |
| `-RemoveQueue` | Remove the queue, the scheduled task and the installed bridge. |

> [!WARNING]
> The installer creates a self-signed code-signing certificate and adds it to `LocalMachine\Root` and `LocalMachine\TrustedPublisher` so Windows accepts the test-signed package. Only use it on a machine you control.

## Command-line reference

```text
lbp810_spool.exe <COMMAND> [ARGS]
```

| Command | Description |
|---|---|
| `--list` | List print queues visible to Windows. |
| `--usb-scan` | List USBPRINT interfaces with their IEEE 1284 ID and status byte. Does not print. |
| `--capt-info` | Query CAPT printer info (device id, firmware, block size). Does not print. |
| `--print-test` | Print a built-in A4 test page. |
| `--print-pbm <FILE>` | Print one or more concatenated binary PBM (`P4`) images, up to 4960×7040 at 600 dpi. |
| `--print-pwg <FILE>` | Print a PWG Raster file directly, e.g. to retry a `*.failed.pwg` job. |
| `--pwg-to-pbm <IN> <OUT>` | Convert PWG Raster to multi-image PBM without printing. |
| `--watch-capture [--console]` | Run the bridge. Logs to `bridge.log`, or to the console with `--console`. |
| `--send-preencoded-capt <QUEUE> <FILE>` | Send an already CAPT-encoded file to a queue as RAW data. |

Supported PWG input: `Srgb_8`/`Rgb_8`/`AdobeRgb_8`, `Sgray_8`/`Gray_8`, `Black_8`, `Sgray_1`/`Gray_1`, `Black_1` at 300 or 600 dpi. Media smaller than A4 is placed at the top-left corner; larger media is cropped.

## Limitations

- A4 only (CAPT paper code `0x09`). Other sizes are padded or cropped to A4.
- Windows' PWG renderer sends sRGB at 300 dpi for this package even though the PDC declares `Sgray_8` at 600 dpi, so fine detail is limited to 300 dpi.
- Printer status is reported in `bridge.log`, not in the Windows queue UI; the transport is a scheduled task rather than a port monitor.
- The package is test-signed. It is not suitable for distribution to machines you do not control.

## Credits

- [darkvision77/libcapt](https://github.com/darkvision77/libcapt) (BSD-2-Clause): CAPT protocol and SCoA compression. Keep its license notice when distributing binaries.
- [caxapyk/capt_lbp810-1120](https://github.com/caxapyk/capt_lbp810-1120): historical reference for the LBP-810 on Linux. No GPL code was copied.
- [Microsoft printer driver documentation](https://learn.microsoft.com/en-us/windows-hardware/drivers/print/).

Canon and LBP are trademarks of Canon Inc. This project is not affiliated with or endorsed by Canon.
