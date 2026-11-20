# Data quality

Use `ost acquire` or the Python camera API to bin, accumulate and save frames while preserving quantitative data quality. Binning and accumulations sum signal; they do not average it. Check saturation before using the result quantitatively, keep the raw baseline offset and subtract a dark acquired with exactly the same settings and thermal state. See the [feature support matrix](support-matrix.md) for supported acquisition settings.

## Binning sums charge and can saturate earlier

Hardware binning adds the charge from adjacent pixels before readout. A 2 x 2 bin therefore represents four sensor pixels, not their mean. This can improve readout-limited signal-to-noise and reduce readout time and data size, but costs spatial resolution (ST-133 Controller Manual Version 3.B, pages 59-60; MicroMAX System User Manual Version 6.C, pages 67-68).

The combined charge can exceed the shift-register or output-node capacity before any constituent pixel exceeds its own well capacity. The resulting saturation or blooming cannot be repaired after readout. Large bins on a bright background are especially vulnerable (ST-133 Controller Manual Version 3.B, pages 60-61; MicroMAX System User Manual Version 6.C, pages 68-70). The 16-bit output can also reach 65535; a saturated spectrum is not a quantitative measurement.

Start with less light, a shorter exposure or fewer binned pixels when a frame approaches saturation. Inspect both single-frame maxima and flat-field behavior rather than trusting the mean. Software binning or a combination of limited hardware and software binning is a separate host-side operation with an explicit output-type rule; openst133 does not silently substitute it for hardware binning.

See [Readout modes and ROI](modes.md) for the supported geometry controls.

## Accumulations return int32 sums

With `--accumulations N`, openst133 reads N camera frames and sums corresponding pixels into one `int32` frame. This is a host-side sum of complete camera transfers, not an average and not extra analog integration in one pixel. N can be 1, 3 or 10; N = 1 returns an ordinary `uint16` frame. The accumulation count is saved in the SPE metadata. WinSpec and WinView likewise describe accumulations as repeated exposures and provide a LONG INTEGER data type (WinSpec/32 User Manual Version 2.5.C, pages 183 and 215; WinView/32 User Manual Version 2.5.G, pages 141 and 170-171).

For unsigned 16-bit constituent samples, the theoretical accumulated ceiling is `N x 65535`: 196605 for three accumulations and 655350 for ten. These values fit comfortably in signed int32. They are output-range bounds, not proof that the detector or ADC avoided saturation. Saturated constituent data stays saturated in the sum.

In the [hardware check](hardware-check.md), `single.frame.saturated_uint16_pixels` counts samples at 65535 in the single `uint16` acquisition. `accumulated.frame.summed_output_maximum_dn` reports the largest int32 sum and `accumulated.frame.theoretical_maximum_dn` reports `N x 65535`. The accumulated summary deliberately has no `saturated_uint16_pixels` field: an accumulated value of exactly 65535 can be the sum of unsaturated samples, and the saved sum cannot reveal whether a constituent frame saturated. Acquire an unsaturated single-frame reference with otherwise matching settings when that distinction matters.

## Gain and readout rate need measurement

Controller Gain 1, 2 and 3 are vendor controller settings, not calibrated conversion factors or display-brightness levels. Analog gain changes the electrons required for one ADU and the usable signal range, but the electrons per ADU for this detector and each setting must be measured before making a quantitative conversion (ST-133 Controller Manual Version 3.B, page 57; MicroMAX System User Manual Version 6.C, pages 70-71). Keep the selected gain and its acquisition settings with the data. The hardware check's illuminated characterization can estimate conversion gain; its gain-selection check alone does not.

Faster digitization generally increases CCD read noise. The 100 kHz mode is therefore a lower-noise candidate, not an automatic improvement for every measurement: it also lengthens readout, and its benefit must be measured against 1 MHz under matched exposure, temperature, geometry, gain and illumination conditions (ST-133 Controller Manual Version 3.B, page 62; MicroMAX System User Manual Version 6.C, page 71). openst133 currently permits 100 kHz only for the unbinned full frame; see [Readout modes and ROI](modes.md).

## Acquire and subtract a matching dark

1. Let the camera reach a stable operating temperature and the desired acquisition readiness. A target request or a transient software lock alone does not establish thermal stability; see [Cooling](cooling.md).
2. Choose the exposure, ROI, binning, accumulation count, readout rate and gain. Acquire several dark frames with light blocked, using Disabled Closed where appropriate. Every one of those settings and the detector temperature must match the later signal acquisition. Dark charge and its pattern depend on exposure and temperature (ST-133 Controller Manual Version 3.B, pages 55-56; MicroMAX System User Manual Version 6.C, pages 59-60).
3. Keep the dark and signal frames' natural nonzero baseline offset. Do not adjust the raw baseline to zero or rescale one frame to match the other's mean. The offset preserves low-level signal variations (MicroMAX System User Manual Version 6.C, page 59).
4. Inspect the dark frames for drift and saturation. If they are stable, form a reference with an explicitly chosen host-side statistic such as their per-pixel mean. Subtract that matching reference from each signal frame using a signed or floating-point array, leaving noise residuals signed rather than clipping them or allowing unsigned arithmetic to wrap them. Preserve the raw dark and signal data and their metadata.
5. Acquire a fresh dark whenever temperature or any acquisition setting changes. A dark from another exposure, geometry, accumulation count, rate or gain does not establish the same background.

For example, these two commands keep exposure, full-frame 2 x 2 geometry, accumulations, rate and gain identical, changing only shutter operation:

```sh
COMMON="--exposure 0.1 --bin 2x2 --accumulations 3 --rate 1M --gain 2"
ost acquire $COMMON --closed --out dark.spe
ost acquire $COMMON --out sample.spe
```

The correction is host processing. In Python, `sample.astype(np.float64) - dark_mean` preserves signed residuals; record the dark-averaging method alongside the result. A summed dark is required for summed signal frames, so the example uses the same three accumulations for both.

## Temperature reads wait for acquisition

Temperature calls and other device operations run one at a time per camera. A temperature read issued while an acquisition or stream is running blocks until that operation returns, so a monitoring display can retain its previous value during a continuous stream. If waiting is unacceptable, skip the temperature call until acquisition has stopped rather than expecting concurrent telemetry. Host-side getters for already stored settings do not wait, but they do not provide a new detector temperature measurement.
