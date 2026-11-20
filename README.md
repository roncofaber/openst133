# openst133

openst133 is an independent, MIT-licensed C library, `ost` command-line program and Python package for selected Princeton Instruments ST-133 USB camera interfaces. It acquires spectroscopy frames and reads and writes SPE data files.

**This is early development software.** Current hardware evidence comes from the tested configuration below. Replay and simulation do not establish camera compatibility.

## Start here

```sh
python3 -m venv ~/openst133-venv && . ~/openst133-venv/bin/activate
python -m pip install openst133
openst133 setup --dat PI133B.DAT --dll PICHIPDB.dll   # once: USB access and startup data from your licensed WinSpec files
ost acquire --exposure 0.1 --out first-frame.spe
```

Complete the [safety checklist](docs/user/safety.md) before the acquire step.

- **Linux:** follow the [quickstart](docs/user/quickstart.md) for each step with its expected output and the safety checklist.
- **Windows:** follow the [Windows setup guide](docs/user/windows.md), which adds binding Windows' built-in WinUSB driver; no camera workflow has been tested on Windows.
- **All platforms:** use the [user guide](docs/user/README.md) for acquisition, readout modes, cooling, Python and integrations.

Real-camera acquisition requires private [startup data](docs/user/startup-data.md) built from your own legally obtained, licensed WinSpec installation. The project never distributes this startup data.

## Supported hardware and status

| Configuration or capability | Status |
| --- | --- |
| Linux, ST-133 USB 2.0 (`0bd7:a010`), Spec-10:100B with EEV 100x1340B detector (detector 98, 1340 x 100 pixels) | Tested configuration: discovery, identification, temperature, frames, series, streams, selected ROI and binning modes, 100 kHz and 1 MHz readout, and SPE output have been observed |
| Spec-10:400B and Spec-10:400BR | Development targets; not observed |
| Windows | Wheels are built and tested in CI; no Windows camera run is established |
| SPE 2.x and SPE 3.0 files | Read and write support; pixel values are uncalibrated |
| Other camera configurations | Not verified; the same USB ID alone does not establish compatibility |

No workflow is recorded as user-tested. Cooled operation has not been validated. External Sync and PreOpen are unsupported, and unsupported detector or readout combinations are refused rather than guessed. See the [feature support matrix](docs/user/support-matrix.md) for exact behavior and evidence limits.

## How it was developed

openst133 is a clean-room implementation built with the [theseus](https://github.com/roncofaber/theseus-lab) framework. One group of agents examined a licensed WinSpec installation and the camera's USB traffic and wrote protocol reports in their own words. A separate group of agents, which never saw vendor code, wrote openst133 from those reports and public documentation. The repository contains no vendor code, firmware or data files.

## Legal

openst133 is an independent project, not affiliated with or endorsed by Teledyne Princeton Instruments or Roper Scientific. Product names are trademarks of their respective owners and are used only to identify compatible hardware.

No vendor software, firmware or vendor data files are included or downloaded by this project. Real-camera use requires your own legally obtained, licensed WinSpec installation, for example the copy supplied with the camera, from which you build private startup data. You are responsible for complying with the WinSpec license. openst133 never distributes the resulting startup data.

openst133 is provided without warranty. Use it at your own risk with valuable hardware, and complete the [safety checklist](docs/user/safety.md) before camera use.

## License

openst133 is available under the [MIT License](LICENSE). Copyright 2026 DATM group, The Molecular Foundry, Lawrence Berkeley National Laboratory. Runtime and bundled components are listed in the [third-party notices](THIRD_PARTY_NOTICES.md).
