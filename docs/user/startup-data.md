# Startup data

Real-camera acquisition requires startup tables built from your own legally obtained, licensed WinSpec installation. openst133 does not include or download vendor software, firmware or vendor data files, and never distributes the resulting startup data. You are responsible for complying with the WinSpec license.

The input is two files from WinSpec: `PI133B.DAT` and `PICHIPDB.dll`. openst133 does not need a working WinSpec, and providing these two files is up to you, under your WinSpec license. Any copy works; the usual source is the WinSpec program folder on the PC that ran the camera, typically `C:\Program Files\PI Acton\WinSpec` or similar. Only detector 98 is supported. Keep the source files and generated output private.

## Build it

`openst133 setup` builds the startup data as part of the guided setup. Pass the files individually or as a folder containing both:

```sh
openst133 setup --dat /path/to/PI133B.DAT --dll /path/to/PICHIPDB.dll
openst133 setup /path/to/folder
```

Run without either, `openst133 setup` asks for the two paths. `openst133 startup-data` takes the same arguments for the startup-data step alone.

If WinSpec is not installed anywhere, one route is to install it from your CD or zip once in any Windows virtual machine, with no camera or USB passthrough, and copy the two files out. openst133 does not unpack WinSpec installers itself.

Successful generation prints the output path, byte count and eight generated tables. The tool checks the input structures and refuses unsupported layouts, a missing detector or inconsistent records rather than guessing.

**Keep `st133-startup.bin` and its `.sha256` file on your machine. Never commit, share or attach them to reports.**

## Where it is stored

| System | Default file |
| --- | --- |
| Linux | `~/.local/share/openst133/firmware/st133-startup.bin` |
| Windows | `%APPDATA%\openst133\firmware\st133-startup.bin` |

A `.sha256` sidecar is written next to the file. `OPENST133_STARTUP_DATA` overrides the full file path, and `OPENST133_DATA` moves the folder to `$OPENST133_DATA/firmware/`. If you write it elsewhere with `--output FILE`, set `OPENST133_STARTUP_DATA` to the same path in every later session; otherwise camera commands and `openst133 setup` look only at the default path.

A missing or mismatched file stops the camera open before any camera traffic.

## Verify the file

These checks confirm that the file matches its sidecar; they are not a camera test. On Linux, a passing check prints `st133-startup.bin: OK`:

```sh
cd ~/.local/share/openst133/firmware
sha256sum --check st133-startup.bin.sha256
```

On Windows, the two printed SHA-256 values must match:

```bat
certutil -hashfile "%APPDATA%\openst133\firmware\st133-startup.bin" SHA256
type "%APPDATA%\openst133\firmware\st133-startup.bin.sha256"
```

Continue with the [Linux quickstart](quickstart.md#4-run-the-complete-check-without-a-camera) or [Windows setup](windows.md#6-find-the-interface-with-openst133), and complete the [safety checklist](safety.md) before using the camera.
