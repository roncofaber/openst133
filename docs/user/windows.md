# Windows setup

No camera command has run on Windows yet; this guide is checked against the current source and packaging rather than presented as a tested Windows hardware workflow.

Use this numbered path to install openst133 beside your licensed WinSpec installation, acquire one frame and return the camera to WinSpec. Commands are for **Command Prompt** (`cmd.exe`), not PowerShell. Nothing outside Windows and Python needs to be downloaded.

Before starting, have:

- 64-bit Windows 10 or 11 and administrator rights for changing or restoring the USB driver. Run the camera commands as your normal user.
- The files `PI133B.DAT` and `PICHIPDB.dll` from your legally obtained, licensed WinSpec, usually in its installed folder on this PC, represented here by `C:\WinSpec`. See [Startup data](startup-data.md) if WinSpec is not installed anywhere.
- The supported ST-133 USB 2.0 interface `0bd7:a010` with detector 98, 1340 x 100 pixels. Other detectors are refused.
- 64-bit CPython 3.10 to 3.14 from [python.org](https://www.python.org/downloads/windows/) with the `py` launcher.

## 1. Install openst133

```bat
py -m venv "%USERPROFILE%\openst133-env"
"%USERPROFILE%\openst133-env\Scripts\activate"
python -m pip install openst133
```

**Success:** pip prints `Successfully installed ... openst133-0.1.0` with NumPy. The wheel bundles `openst133.dll`, `libusb-1.0.dll` and `ost.exe`; no compiler is needed. **Fix:** if pip reports that no matching distribution was found, check that `python -c "import struct; print(struct.calcsize('P') * 8)"` prints `64`; 32-bit Python is unsupported.

## 2. Check the installed commands

```bat
ost --help
openst133 --help
```

**Success:** `ost` lists `list`, `info`, `spe-info`, `spe-dump`, `temperature` and `acquire`; `openst133` lists `list`, `view`, `acquire`, `startup-data`, `setup`, `setup-udev` (Linux only), `check` and `temperature-log`. **Fix:** `ost is not recognized` means the environment is not active; run `"%USERPROFILE%\openst133-env\Scripts\activate"` in each new Command Prompt.

## 3. Generate and verify the private startup data

`openst133 setup` finds WinSpec in its usual installed folders. Otherwise pass a folder containing both files, as below, or the files themselves with `--dat` and `--dll`:

```bat
openst133 setup "C:\WinSpec"
certutil -hashfile "%APPDATA%\openst133\firmware\st133-startup.bin" SHA256
type "%APPDATA%\openst133\firmware\st133-startup.bin.sha256"
```

**Success:** the startup-data section prints `wrote ...` with `8 tables`, and the digest printed by `certutil` matches the sidecar. Until the camera is bound to WinUSB in step 5, the setup reports it as not ready; that is expected. Keep the input files, generated binary and sidecar private; never commit or share them. **Fix:** point the first argument at the actual installed folder containing both input files, not the WinSpec shortcut or installer. If it refuses an input layout or detector, follow that refusal rather than substituting files. See [Startup data](startup-data.md) for the output location rules, including `OPENST133_DATA`, `OPENST133_STARTUP_DATA` and `--output`.

## 4. Prepare the camera and record its WinSpec driver

Close WinSpec, other acquisition programs and every VM that can hold the camera. Complete the [safety checklist](safety.md) before connecting or powering the controller, and connect and power it exactly as that checklist directs. Open **Device Manager**, locate the existing ST-133 or WinSpec camera entry, open **Properties > Driver**, and record its current **Driver Provider** and **Driver Version** so you can return to it later.

**Success:** the device entry is present, its current driver details are recorded, and no WinSpec, VM or acquisition process is using it. **Fix:** if no entry appears, stop and check the power and USB path, including VM passthrough; do not select a similar-looking device in later steps.

## 5. Bind the built-in WinUSB driver

openst133 talks to the camera through libusb, which needs Windows' own WinUSB driver on the interface. WinUSB ships with Windows; this follows Microsoft's documented [installation by system-provided device class](https://learn.microsoft.com/en-us/windows-hardware/drivers/usbcon/winusb-installation#installing-winusb-by-specifying-the-system-provided-device-class). It replaces the driver WinSpec uses until you restore it in step 8.

1. In Device Manager, right-click the ST-133 entry from step 4 and choose **Update driver**. Open **Properties > Details > Hardware Ids** first if you need to confirm that it shows `VID_0BD7&PID_A010`.
2. Choose **Browse my computer for drivers**, then **Let me pick from a list of available drivers on my computer**.
3. In the list of device types, select **Universal Serial Bus devices** and choose **Next**.
4. Under **Manufacturer** select **WinUsb Device**, under **Model** select **WinUsb Device**, then choose **Next**.
5. Confirm the **Update Driver Warning** with **Yes**.

**Success:** Windows reports that it updated the driver, and the camera now appears under **Universal Serial Bus devices** as **WinUsb Device**. **Fix:** if you changed the wrong device, use step 8 on that device to restore its previous driver before continuing.

## 6. Find the interface with openst133

<a id="discover-and-acquire"></a>

Use a Command Prompt with the environment active, where `OPENST133_NO_HARDWARE` is not `1`:

```bat
openst133 setup
```

The setup finds the camera and opens and closes it without sending camera commands, which confirms the WinUSB binding. **Success:** the access section reports `access works` and the Result section prints `ready.` followed by the first acquisition command. **Fix:** if no camera is found, close WinSpec and every VM and reconnect only when the safety checklist permits; if it cannot open the camera, repeat step 5 on the entry with `VID_0BD7&PID_A010`.

## 7. Save and inspect a first camera frame

This acquisition sends startup writes and requests a -120 C temperature target. A target request is not proof that the detector is cooled; use your normal optical and detector-safety procedure.

```bat
ost acquire --exposure 0.1 --out first-frame.spe
ost spe-info first-frame.spe
```

**Success:** the result is `acquired 1340x100 frame, exposure 0.100 s, normal shutter, gain 2, saved first-frame.spe`, and inspection reports SPE version 3, uint16 data, 1340 x 100 pixels and one frame. **Fix:** for missing or rejected startup data, return to step 3; for `permission denied` or driver errors, repeat steps 5 and 6; for a busy interface, close WinSpec, VMs, viewers and other `ost` commands. Do not use administrator rights to bypass a wrong driver binding.

## 8. Switch the camera back to WinSpec

Stop acquisition and close every openst133 program. With the camera still connected, open Device Manager as an administrator and follow the [Microsoft Device Manager driver instructions](https://support.microsoft.com/en-us/windows/update-drivers-through-device-manager-in-windows-ec62f46c-ff14-c91d-eead-d7126dc1f7b6):

1. Open the **WinUsb Device** entry, then **Properties > Driver > Roll Back Driver**, supply the requested reason and confirm with **Yes**.
2. Restart Windows or the device if Windows requests it, then reopen the Driver tab.
3. If **Roll Back Driver** is unavailable, choose **Update driver > Browse my computer for drivers > Let me pick from a list of available drivers on my computer**, select the original ST-133 or WinSpec driver recorded in step 4, and choose **Next**. If that driver is not offered, restore it from your licensed WinSpec or camera installation media rather than selecting an unrelated driver.

**Success:** the Driver tab again shows the original provider and version recorded in step 4, and WinSpec opens the camera with its established configuration. **Fix:** if WinSpec still reports a driver or connection error, close openst133 and all VMs, confirm the restored provider, and reconnect or restart only as the safety checklist permits.

## Troubleshooting

- **A DLL is missing:** reinstall with `python -m pip install --force-reinstall openst133` in the active environment. Do not use a 32-bit Python or copy DLLs from WinSpec.
- **`openst133 shared library not found; set OPENST133_LIBRARY to its path`:** the package was installed without its bundled library. Reinstall it with `py -m pip install --force-reinstall openst133`.
- **`permission denied opening camera USB interface ...`:** the WinUSB binding from step 5 is missing or another program holds the camera. Running as administrator is not the fix.
- **Startup data is not found at the expected path:** the default is `%APPDATA%\openst133\firmware\st133-startup.bin`. To use a custom file, generate it and keep the same variable for later camera commands:

  ```bat
  openst133 startup-data "C:\WinSpec" --output "D:\private\st133-startup.bin"
  set "OPENST133_STARTUP_DATA=D:\private\st133-startup.bin"
  ```

- **`OPENST133_NO_HARDWARE=1 forbids opening real hardware`:** clear the variable only in this intentional hardware Command Prompt with `set OPENST133_NO_HARDWARE=`, then retry. Leave the guard enabled in scripts and automated sessions.
- **The camera works in openst133 but not WinSpec, or the reverse:** only the currently bound driver is available. Repeat step 5 for openst133 or step 8 for WinSpec.

Next: [Acquire frames](acquire.md).
