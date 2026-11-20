# Python logging

Control Python command diagnostics with `--log-level LEVEL` and `--log-file PATH`:

```sh
openst133 acquire --simulate --exposure 0.1 --out frame.spe --log-level INFO
```

The package uses the standard-library `logging` module with the logger hierarchy `openst133`, `openst133.camera` and `openst133.viewer`. Library modules install only a `NullHandler`, so importing the package adds no output until your application configures logging. Applications retain control of handlers and propagation.

## Command options

`openst133 view`, `openst133 acquire` and `openst133 check` accept these options after the command name; the native `ost` command does not use Python logging:

- `--log-level LEVEL` selects `DEBUG`, `INFO`, `WARNING`, `ERROR` or `CRITICAL`. The default is `WARNING`.
- `--log-file PATH` appends UTF-8 records to the named plain file. Without it, records go to stderr. Existing status, result and error messages remain in their original streams.

Routine `INFO` and `DEBUG` records are silent at the default level. An `ERROR` record can appear on stderr when an operation fails. These options configure only local stderr or file handlers, never syslog or the systemd journal.

Write simulated viewer records to a file with:

```sh
openst133 view --simulate --log-level DEBUG --log-file viewer.log
```

The hardware check accepts the same options without accessing a camera in simulation mode:

```sh
openst133 check --simulate --output simulated-check --log-level INFO --log-file smoke.log
```

Each file record includes its timestamp, logger name, severity and message. A file is created even when the selected level filters every record. Repeated runs append to an existing file.

## Record contents

At `INFO`, camera backend open and close records identify only the simulated or native backend and frame geometry. Applied exposure, temperature, shutter, ROI and binning settings are recorded after the corresponding operation succeeds. Every completed single-frame acquisition records exposure, shutter state, frame count, elapsed duration and frame mean, minimum and maximum. Saving an SPE file also records its frame count and destination path.

At `DEBUG`, acquisition start, frame receipt and worker completion provide per-step detail. Camera and acquisition failures are logged at `ERROR`; native camera failures include the unchanged text returned by `ost_camera_last_error` through the Python binding. Abort records do not imply a completed frame.

The openst133 log calls do not include serial numbers, camera identification dumps or image data. They contain numeric frame summaries, user-selected settings and destination paths.

## Configure another application

Application code can configure the `openst133` logger or its children without changing library code:

```python
import logging

handler = logging.FileHandler("camera.log", encoding="utf-8")
handler.setFormatter(logging.Formatter("%(asctime)s %(name)s %(levelname)s %(message)s"))
logger = logging.getLogger("openst133")
logger.setLevel(logging.INFO)
logger.addHandler(handler)
```

Remove and close application-owned handlers during shutdown when appropriate. Before enabling file, network, syslog or journal handlers, follow your site's privacy and log-retention policies and verify where those handlers send records.
