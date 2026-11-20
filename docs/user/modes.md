# Readout modes and ROI

Use the readout options of `ost acquire` to select full vertical binning, a region of interest, integer binning or the digitization rate:

```sh
ost acquire --fvb --exposure 0.1 --out spectrum.spe
```

The supported sensor is 1340 x 100 pixels. By default `ost acquire` reads the full frame at 1 MHz. Every command on this page sends camera traffic.

| Option | Effect |
| --- | --- |
| `--fvb` | Full vertical binning: sum every row of the region into one spectrum; without `--roi`, produce a 1340-point spectrum |
| `--roi X,Y,W,H` | Select a region of interest in sensor pixels; without `--bin` or `--fvb`, read it unbinned |
| `--bin XxY` | Apply horizontal binning X and vertical binning Y within the region, or within the whole sensor without `--roi` |
| `--rate 1M\|100k` | Select 1 MHz or 100 kHz digitization |

The output size is the region divided by the binning: `(W / X) x (H / Y)`.

## Combining the geometry flags

`--roi` chooses the region; `--fvb` and `--bin` choose how it is binned.

| Flags | Readout | Output |
| --- | --- | --- |
| `--fvb` | Whole sensor, binning 1 x 100 | 1340 x 1 |
| `--fvb --roi X,Y,W,H` | Region, binning 1 x H | W x 1 |
| `--fvb --bin Nx1` | Whole sensor, binning N x 100 | 1340/N x 1 |
| `--fvb --roi X,Y,W,H --bin Nx1` | Region, binning N x H | W/N x 1 |
| `--roi X,Y,W,H --bin XxY` | Region, binning X x Y | W/X x H/Y |
| `--bin XxY` | Whole sensor, binning X x Y | 1340/X x 100/Y |

With `--fvb`, vertical binning always equals the number of rows in the region, so `--bin` only adds horizontal binning. `--fvb --bin 2x1` is equivalent to `--bin 2x100`; `--bin Nx<rows>` is also accepted with `--fvb`. Any other vertical factor conflicts with `--fvb` and is refused before the camera opens.

W must be divisible by X and H by Y. A partial bin fails with `invalid argument` and the message `ROI and binning must fit the sensor with positive extents and bins` before acquisition starts. Unsupported geometry is refused rather than replaced with another readout.

## Examples

```sh
ost acquire --fvb --exposure 0.1 --out spectrum.spe
ost acquire --fvb --roi 200,0,800,100 --exposure 0.1 --out spectrum-200-999.spe
ost acquire --roi 0,40,400,20 --exposure 0.1 --out roi.spe
ost acquire --bin 4x1 --exposure 0.1 --out binned.spe
ost acquire --fvb --bin 2x1 --exposure 0.1 --out spectrum-670.spe
ost acquire --rate 100k --exposure 0.1 --out slow.spe
```

The second command sums all 100 rows of columns 200 to 999 into an 800-point spectrum. The fifth sums all rows and pairs of columns into a 670-point spectrum.

A 100 kHz acquisition loads its own speed table into the controller. The next acquisition, at either rate, first reruns the camera startup sequence to reload the 1 MHz table, which adds about 2.5 s. This applies to each frame of `--count` at 100 kHz; frames inside one `--stream` run share a single reload.

## In Python and the GUIs

- Python: `set_readout(x, y, width, height, x_binning, y_binning)`, `set_full_vertical_binning()`, `set_roi(x, y, width, height)`, `set_binning(x_binning, y_binning)` and `set_readout_rate(rate_hz)` on `RealCamera`; see [Python](python.md#settings).
- Viewer: the **Region**, **Bin** and **Sum rows (FVB)** controls, which combine like the flags above; see [Viewer](viewer.md).
- ScopeFoundry: the `roi_*`, `bin_*` and `full_vertical_binning` settings; see [ScopeFoundry](scopefoundry.md#settings).
