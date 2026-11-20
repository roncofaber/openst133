# Feature support matrix

Use this matrix to check hardware and feature support before acquisition. An accepted request has passed API validation; it does not establish physical behavior for every accepted combination.

## Hardware and validation

| Capability | Current API support | Validation status or limit |
| --- | --- | --- |
| ST-133 USB 2.0 with EEV 100x1340B | Detector ID 98, 1340 x 100 pixels, is supported | Selected acquisition, readout and temperature operations have been checked on Linux |
| Other detectors and controllers | Unsupported configurations are refused | No compatibility is claimed |
| Linux and Windows | Native and Python interfaces are available for both platforms | Linux has hardware checks; Windows has build checks but no hardware run |
| Measurement values | SPE stores detector values and acquisition metadata | Pixel values are uncalibrated; encoded exposure time is not a measurement of physical integration |

## Camera access

Every camera command builds its settings from the requested exposure, target, geometry, readout rate, gain and accumulations, and the library refuses values outside the ranges below and unsupported combinations rather than substituting others. Startup data from `openst133 setup` is required. Settings have been checked on Linux for selected configurations, not every accepted combination. Listing devices and reading SPE files send no camera commands.

## Exposure and temperature

| Capability | Current API support | Validation status or limit |
| --- | --- | --- |
| Exposure request | 0 to 60 s, rounds to the nearest millisecond (the Python API requires more than 0 and rounds up to whole milliseconds) and otherwise shortens to a representable value; for example 0.333 s becomes 0.332 s and 10 s becomes 9.984 s | The SPE stores the effective encoded value. The 60 s request limit is not a validated physical maximum |
| Exact exposure | `--strict-exposure` refuses a request that would be shortened | No alternative duration is silently substituted when strict handling is requested |
| Zero and short requests | Zero is accepted as a zero-request or dark acquisition; short requests within the range are supported | Encoded zero does not establish zero physical integration, and requests below the published 5 ms minimum have unmeasured integration semantics |
| Temperature target | -152 to -25 C in half-degree steps and can save the selected target for later openings | A target is a request, not proof of cooling. Cooled operation is unvalidated |
| Temperature lock | The temperature logger derives `locked` and readiness from target proximity over time | These are host judgments, not a controller hardware-lock indication; cooled lock behavior is unvalidated |

## Timing and shutter modes

| Capability | Current API support | Validation status or limit |
| --- | --- | --- |
| Free Run | Supported acquisition timing; there is no separate timing-mode selector | Selected frames and streams have been checked on Linux |
| Normal and Disabled Closed shutters | Available | Selected acquisitions checked on Linux; exact shutter-compensation timing is unmeasured |
| Disabled Opened shutter | Available as `--shutter opened` | Selected acquisitions checked on Linux; ambient-light handling remains the user's responsibility |
| External Sync and PreOpen | Unsupported; `--shutter preopen` returns an unsupported result | Triggered acquisition, polarity, delay and first-frame behavior are unvalidated |
| Continuous Cleans | No API control | Cleaning latency and image effects are unvalidated |
| Fast and Safe collection modes | No vendor-named mode selector; streaming uses queued transport and host counters | Do not interpret host queue behavior as either vendor mode |

## Readout and data

| Capability | Current API support | Validation status or limit |
| --- | --- | --- |
| Single frames and series | Single frames, and `--count N` series acquired as consecutive single frames | Selected frame counts checked on Linux |
| Full frame | 1340 x 100 pixels | Selected acquisitions checked on Linux |
| Full vertical binning | FVB over the full sensor or a region, with optional horizontal binning | Selected configurations checked on Linux; other geometries are not individually hardware-validated |
| ROI and integer binning | One rectangular ROI with positive extents, sensor bounds and complete bins | Partial bins and unsupported geometries are refused, never replaced with software averaging; see [Readout modes and ROI](modes.md) |
| Overlapping ROIs | Unsupported | Published USB 2.0 documentation marks overlapping ROIs unsupported |
| Readout rate | 1 MHz and 100 kHz; 100 kHz is restricted to the full unbinned frame; each acquisition after a 100 kHz one first reruns the startup sequence to reload the speed table, adding about 2.5 s | Switching 1 MHz, 100 kHz and back, single and streamed, checked on Linux. Noise performance must be measured for the experiment |
| Controller Gain | 1, 2 and 3 (default 2) | Settings have been exercised, but electrons-per-ADU conversion and saturation levels are not calibrated |
| Accumulations | 1, 3 or 10, summing complete uint16 frames into int32 output. Counts above 1 cannot be combined with streaming | Constituent frames can saturate even when the int32 sum does not; see [Data quality](data-quality.md) |
| SPE output | Single frames and streams use uint16 pixels; accumulations use int32 LONG pixels | Metadata records effective acquisition settings, not independent physical calibration |

## Streaming

Streaming supports the geometries above. The counters in [Stream frames](stream.md#read-the-counters) distinguish complete frames, partial data, host pacing and Python consumer drops.

The following cadence measurements are examples from one Linux host, not guaranteed rates for another host or configuration. Other geometries and exposures have not been timed.

| Configuration | Requested exposure | Measured cadence | Run result |
| --- | --- | --- | --- |
| 1340 x 100 full frame | 0.1 s | 0.237 s per frame | Requested frames delivered in 10-frame and 20-frame runs |
| `--fvb`, 1340 x 1 | 0.001 s | 4.36 ms, about 229 spectra/s | 1000 spectra, zero host stalls and zero lagging frames |
| `--bin 2x100`, 670 x 1 | 0.001 s | 4.83 ms, about 207 spectra/s | 1000 spectra, zero host stalls and zero lagging frames |

## Unsupported USB features

| Capability | Current API support | Validation status or limit |
| --- | --- | --- |
| TTL I/O | No control interface | Published USB 2.0 documentation marks TTL I/O unsupported |
| Virtual Chip | No control interface; ROI, FVB and streaming do not select Virtual Chip | Requires suitable frame-transfer hardware and is documented as unsupported over USB 2.0 |
| Reset to NVRAM defaults | No reset operation | Startup uses an explicit startup-data file instead |
| RS170 video output | No video-output interface | Ordinary SPE acquisition is unrelated to analog video output |
| Online Exposure | No Online Exposure operation | Ordinary `ost acquire` waits for the requested frame transfer and is not Online Exposure |

For command syntax and output, use [Acquire frames](acquire.md), [Readout modes and ROI](modes.md), [Stream frames](stream.md) and the [command reference](cli.md).
