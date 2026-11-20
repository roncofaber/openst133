# Stream frames

Add `--stream` to `ost acquire` to collect frames continuously without reopening the camera:

```sh
ost acquire --exposure 0.001 --fvb --stream --count 1000 --out spectra.spe
```

Streaming supports every supported geometry, including FVB spectra. Every command on this page sends camera traffic.

## From the command line

```sh
ost acquire --exposure 0.1 --stream --count 20 --out stream.spe
ost acquire --exposure 0.1 --stream --out last.spe
ost acquire --exposure 0.001 --fvb --stream --count 1000 --out spectra.spe
```

With `--count N`, acquisition stops after N frames and saves all of them. Without a count, it runs until Ctrl-C and saves the last complete frame. At the end, `ost` sends the stop sequence and prints a summary on stderr:

```text
ost: stream: N frames delivered, B partial bytes discarded, S host stalls, L lagging frames, count reached
```

The last field is `stopped on request` after Ctrl-C. Dropped or partial data is reported, never hidden. The options in [Readout modes and ROI](modes.md) can be combined with `--stream`; unsupported combinations are refused. See the [streaming matrix](support-matrix.md#streaming) for limits and measured cadence examples.

## Read the counters

The CLI summary shows `frames_delivered`, `partial_bytes_discarded`, `host_stalls` and `frames_lagged`. After a Python stream, `camera.stream_stats` contains the native counters and `camera.stream_dropped` contains the separate Python consumer count.

| Counter | Meaning |
| --- | --- |
| `frames_delivered` (`N`) | Complete frames passed to the native frame callback, excluding Python frames later dropped before the consumer received them |
| `partial_bytes_discarded` (`B`) | Bytes from an incomplete trailing frame, including cancelled transfer data; partial data is never padded or delivered as a frame |
| `host_stalls` (`S`) | Times no image transfer was ready to keep the pipe busy when the host returned for data; this is a host pacing count, not a dropped-frame count |
| `frames_lagged` (`L`) | Frames whose native callback exceeded the preceding measured frame period; this identifies slow processing, not the number of discarded frames |
| `bytes_received` | Total image bytes seen by the host, including bytes later discarded as a partial frame |
| `transfers_completed` | Image USB input completions; a frame can span multiple transfers |
| `short_transfers` | Image transfers shorter than the requested chunk size; a short transfer is data, not necessarily a dropped frame |
| `max_ready` | High-water mark for completed image transfers waiting when the host returned |
| `max_callback_us` | Longest native frame callback in microseconds |
| `stopped` | Nonzero when a user or callback requested the stop; it does not identify a hardware fault |
| `stream_dropped` | Python only: complete frames discarded because the bounded consumer queue was full; the oldest queued frame is dropped first |

These counters describe host activity, not camera fault codes, and the host cannot count frames lost inside the camera. Compare `frames_delivered` with the requested count and `stream_dropped`; a matching count alone does not exclude device-internal loss. The native counters and Python consumer count describe different stages and must not be added together as one hardware-drop count.

## Respond to drops and stalls

For repeated `host_stalls`, `frames_lagged`, a high `max_ready` or Python `stream_dropped`, stop quantitative work and reduce host demand before repeating the acquisition:

- Close CPU-heavy and disk-heavy background programs and avoid unnecessary processing in the acquisition callback or display loop.
- Use a smaller ROI or more binning to reduce bytes per frame, provided the resulting geometry still answers the measurement question.
- Increase exposure to lower the frame rate when the experiment permits; record the new exposure because this changes measurement timing.

Compare the retry using counters and measured cadence for the same readout. Retain the reported error and host/USB conditions when transfer errors or partial data persist. openst133 uses queued acquisition and does not provide a Fast/Safe mode selector.

## From Python, the viewer and ScopeFoundry

- Python: `camera.stream(frames=None)` on `RealCamera` yields arrays; see [Python](python.md#stream).
- Viewer: **Continuous** streams until **Stop**; see [Viewer](viewer.md).
- ScopeFoundry: measurement mode `live`; see [ScopeFoundry](scopefoundry.md).
