# Third-party notices

openst133 is MIT-licensed (see `LICENSE`). It contains no third-party source code. This file lists the third-party software it uses at run time, the components bundled in its wheels, and the published methods it follows. Each entry was checked against the package's declared license on 2026-11-17.

## Software used at run time (not stored in this repository)

| Package | Used by | License | How it is used |
| --- | --- | --- | --- |
| [libusb](https://libusb.info) | C library | LGPL-2.1-or-later | Dynamically linked. The wheels bundle it (see below); source builds use the system library |
| [NumPy](https://numpy.org) | Python package | BSD-3-Clause (and bundled permissive licenses) | Required dependency |
| [ScopeFoundry](https://www.scopefoundry.org) | `integrations/scopefoundry` | BSD | Optional; imported by the plugin only |
| [QtPy](https://github.com/spyder-ide/qtpy) | `integrations/scopefoundry` | MIT | Optional; installed with ScopeFoundry, imported by the plugin's `close_app` |
| [PySide6](https://doc.qt.io/qtforpython/) | `integrations/scopefoundry` (`gui` extra) | LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0 | Optional; installed by the user for the ScopeFoundry GUI, dynamically loaded, never bundled |
| [pyqtgraph](https://www.pyqtgraph.org) | `integrations/scopefoundry` (`gui` extra) | MIT | Optional GUI dependency |
| [matplotlib](https://matplotlib.org) | `integrations/scopefoundry` (`gui` extra) | PSF-based (matplotlib license) | Optional GUI dependency |
| [Bluesky](https://blueskyproject.io/bluesky/) | `examples/bluesky_detector.py` and its documentation | BSD-3-Clause (upstream LICENSE; PyPI 1.16.0 has an Apache Software License classifier) | Optional; RunEngine plans in user code only, never bundled |
| [ophyd](https://blueskyproject.io/ophyd/) | `examples/bluesky_detector.py` | BSD-3-Clause | Optional; imported at run time for `DeviceStatus`, never bundled |
| [PyMoDAQ](https://pymodaq.cnrs.fr/) | `examples/pymodaq_detector.py` | MIT | Optional; imported at run time by the plugin skeleton, never bundled |
| [PyMoDAQ Data](https://pymodaq.cnrs.fr/) | `examples/pymodaq_detector.py` | MIT | Optional; `pymodaq_data.DataToExport` imported at run time, never bundled |
| [h5py](https://www.h5py.org) | `integrations/scopefoundry` | BSD-3-Clause | Optional; H5 export |
| [emva1288](https://github.com/EMVA1288/emva1288) | `openst133.characterization` | LGPL-3.0 | Optional camera characterization backend; imported at run time when installed, never copied into this repository |

## Components bundled in the wheels

| Component | Where it is | License | How it is distributed |
| --- | --- | --- | --- |
| [libusb](https://libusb.info) 1.0 | `openst133.libs/libusb-1.0-*.so.0` (Linux), `openst133.libs/libusb-1.0*.dll` (Windows) | LGPL-2.1-or-later | Unmodified shared library copied from the build environment by auditwheel or delvewheel; the native openst133 library links to it dynamically and the user may replace it. Each wheel carries its license, authors and source pointer in the `.dist-info/licenses/packaging/licenses/libusb/` folder |
| [MinGW-w64](https://www.mingw-w64.org/) runtime and winpthreads | Linked into `ost.exe` and `openst133.dll` in Windows wheels | ZPL-2.1, MIT and BSD-3-Clause portions | Copyright file in `.dist-info/licenses/packaging/licenses/mingw-w64/copyright` |
| [GCC](https://gcc.gnu.org/) runtime (libgcc) | Linked statically into `ost.exe` and `openst133.dll` in Windows wheels | GPL-3.0-or-later with the GCC Runtime Library Exception | Copyright file in `.dist-info/licenses/packaging/licenses/mingw-w64/gcc-copyright` |

The source distribution bundles no third-party component other than these license texts.

## Packaging and CI tools (not distributed with openst133 source)

| Package | Used by | License | How it is used |
| --- | --- | --- | --- |
| [scikit-build-core](https://scikit-build-core.readthedocs.io/) | Python wheel build | Apache-2.0 | PEP 517 backend that invokes CMake |
| [cibuildwheel](https://cibuildwheel.pypa.io/) | Release CI | BSD-2-Clause | Builds and tests manylinux and Windows wheels |
| [auditwheel](https://github.com/pypa/auditwheel) | manylinux wheel repair | MIT | Bundles required shared-library dependencies into built wheels |
| [delvewheel](https://github.com/adang1345/delvewheel) | Windows wheel repair | MIT | Bundles required DLL dependencies into built wheels |
| [MinGW-w64](https://www.mingw-w64.org/) | Windows CI, Linux cross-build CI | ZPL-2.1, with separately identified public-domain, BSD and LGPL portions | Compiles the C library through the MSYS2 UCRT64 toolchain, and with the Ubuntu `gcc-mingw-w64-x86-64` package in `.github/workflows/ci.yml` |
| [libusb](https://libusb.info) 1.0.30 source | `.github/scripts/build-mingw-libusb.sh` | LGPL-2.1-or-later | Downloaded in CI (SHA-256 checked) and cross-built for MinGW; not stored in this repository. Used only by the CI cross-build check; not distributed |
| [actions/checkout](https://github.com/actions/checkout), [actions/setup-python](https://github.com/actions/setup-python), [actions/upload-artifact](https://github.com/actions/upload-artifact), [actions/download-artifact](https://github.com/actions/download-artifact) | GitHub Actions workflows | MIT | Referenced by version tag or commit hash; run by GitHub, not copied |
| [GitHub CLI](https://github.com/cli/cli) | `packaging/release/publish.sh` | MIT | Preinstalled on the GitHub runner; creates the release |
| [pypa/gh-action-pypi-publish](https://github.com/pypa/gh-action-pypi-publish) | `.github/workflows/release.yml` | BSD-3-Clause | Uploads the sdist and wheels to PyPI with trusted publishing; run by GitHub, not copied |
| [build](https://build.pypa.io/) | `.github/workflows/release.yml` | MIT | Builds the source distribution |
| [manylinux](https://github.com/pypa/manylinux) image `manylinux_2_28_x86_64` | `pyproject.toml` (cibuildwheel) | MIT (build scripts); the image holds AlmaLinux 8 packages under their own licenses | Build environment of the Linux wheels; only the libusb library listed above is copied into them |
| [actionlint](https://github.com/rhysd/actionlint) | Workflow validation (developer tool) | MIT | Optional; checks `.github/workflows/*.yml` before a release |

Binary wheels contain the dynamically linked libusb and required runtime dependencies collected by their repair tools. Their source and license obligations remain those of the respective projects; no third-party source is copied into this repository.

## Methods and references

- Camera characterization (offset, read noise, dark current, linearity, gain from the photon transfer curve) follows the EMVA 1288 standard (European Machine Vision Association, https://www.emva.org) and the Micro-Manager guide "Measuring camera specifications" (https://micro-manager.org/Measuring_camera_specifications).
- The SPE file layout follows the publicly documented WinSpec/WinView SPE 2.x format. The binary header offsets and the SPE 3.0 XML footer (DataHistories/Origin, GeneralInformation/FileInformation) follow the Teledyne Princeton Instruments "SPE 3.0 File Format Specification", Issue 4, Appendix A and chapter 5 (public manual); openst133 metadata inside Origin uses its own namespace `urn:openst133:acquisition:1`.
