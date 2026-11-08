# Simulation: physics assumptions and approximations (ckt-sim-v1)

Every frame is a pure function of its case line (`simulation/case_spec.h`) and the PRNG (`common/prng.h`, `ckt-prng-v1`). Each stage draws from its own named stream, so adding a stream never perturbs the others. Any change to this directory must bump `SIM_VERSION` in `pipeline.h`, because it changes image hashes.

## Stages

| Stage | File | Model |
|---|---|---|
| Ideal vector symbol | `render/raster.h` | The production SVG (`ckey_render_*_svg`), rasterized to mark coverage. The harness rasterizer is independent (SVG 1.1 arc conversion, conservative signed distance with an edge-only supersample). ckey's `svg_raster.h` is the cross-check oracle (S01) and the holdout's alternate pipeline. P0 is also compared with the production native raster. |
| Paint substrate | `pipeline.h` `paint_texture` | Linear reflectances from sRGB colours. Antialiased edges are mixed in sRGB like real renderers (`mix_srgb`). Optional transparent background over a striped surface. |
| Print | `printer/printer.h` | Binary device dots at `print_dpi`, replicated to the texture grid. Signed dot gain via an exact Euclidean distance transform. Clustered-dot 45° halftone for tints. Paper wicking blur. Low-frequency multiplicative paper texture. Ink and paper reflectances. |
| Display | `display/display.h` | Screen pixel grid at `disp_ppi`, mixed in sRGB and emitted linear. RGB/BGR stripes or no subpixels, black-matrix fill factor. Sampled procedurally, so screen–camera moiré and colour aliasing emerge from sampling. Cover-glass reflection. PWM/refresh banding modulated by rolling-shutter row time. |
| Scene | `scene/scene.h` | Image-space clutter: rectangles, stripes, text-like strokes, discs, and code-like distractors (QR-like grids, polar dot rings, fake CKey frames). Surface-space lighting: gradient, soft half-plane shadows, specular glare spots, veiling glare. Surface damage: scratches, tears/missing corners (holes), stickers, smudges, finder covers. A defocused fingertip in image space. |
| Projection | `optics/camera.h` | Pinhole camera with Brown radial distortion (k1, k2). Undistortion uses an 8-px lookup lattice with bilinear interpolation, under 3e-5 mm error in the selftest. Pose is roll/yaw/pitch plus translation. The cylindrical bend is intersected in closed form (ray/cylinder quadratic), convex or concave. Rolling shutter is exact: each row is sampled at its own readout time. |
| Camera sampling | `pipeline.h` `draw_instance` | 4 samples per pixel (R2 low-discrepancy). The surface map is unprojected exactly at pixel corners and interpolated inside the pixel. Pixels wholly off the printed canvas take one sample. Textures are 2 texels per camera pixel. |
| Exposure-time blur | `pipeline.h` `exposure_blur` | Translation as a line kernel of `motion_px`. Below 1 px it folds into the lens Gaussian as its exact variance L²/12. Rotation as an arc about the symbol centre; zoom as a radial segment. **Approximation:** applied in image space over the symbol region, valid for hand-held perspective. Background motion blur is not modelled outside that region. |
| Lens | `optics/psf.h` | Gaussian residual blur, with field curvature evaluated at the symbol's field position (**approximation:** one sigma for the frame). Gaussian or pillbox defocus; the pillbox is convolved exactly over the symbol region and variance-matched outside it. Lateral chromatic aberration (RGB). Vignetting. |
| Sensor | `sensor/sensor.h` | Centre-weighted auto-exposure toward 18% of full well, capped by `max_signal_e × illum`; the shortfall becomes digital gain (high ISO). Poisson shot noise is exact below 40 e⁻ and Gaussian above. Gaussian read noise, per-device fixed-pattern PRNU, full-well clip, ADC quantization. Optional RGGB Bayer with bilinear demosaic. |
| ISP | `sensor/sensor.h` | Digital gain → tone curve (sRGB OETF, filmic, or linear) → Y′ luma (Rec. 709, as the reader receives) → edge-gated luma noise reduction → unsharp mask → area rescale (preview paths) → JPEG-like 8×8 DCT quantization (IJG tables). Full or video range. |

## Ground truth

The ground truth for each frame is the four projected field corners. They use **ckey's pixel convention**, where pixel *(i, j)* has its centre at *(i, j)*, as in `gray_sample` in `core_internal.inc`, so `localization_error` is directly comparable to the reader's quads. `gt_quad_decodes` feeds those corners to the production decoder, for triage only.

## Physical recoverability (`recov-v1`)

Computed per frame in `pipeline.h` (`Simulator::physics` / `classify`) and mirrored in `configs/strata.yaml`:

* `ppm_min`: the worst local px per module over a 5 × 5 field grid. It is the smallest singular value of the projection Jacobian times the module pitch.
* `mtf` at 0.5 cycles/module, combining Gaussian blur, defocus disc (Airy-like `2J1(x)/x`), motion `sinc` and pixel aperture `sinc`.
* `dprime` = mark/background signal (electrons) × MTF / √(signal + read noise²).
* `byte_err_est` = 1 − (1 − site-loss)^8, from occluded plus saturated field fractions. This assumes pseudo-random bit placement, which the specs state for P0/P2/P3.
* **UNRECOVERABLE** if the symbol is cropped, `ppm_min < 1`, MTF × contrast < 1% or d′ < 1, or `byte_err_est` exceeds twice the ECC fraction (more than erasure-assisted decoding could correct).
* **MARGINAL** if `ppm_min < 2`, d′ < 3, or `byte_err_est` exceeds the ECC fraction.

These are engineering estimates. They keep impossible inputs out of reader failure rates; they do not prove that a MARGINAL case is recoverable.

## Known limitations

* The device models are generic phone-class parameterizations and **have not yet been validated against real captures** (SPEC §5; see `certification/qualification/real_device_protocol.md`).
* Diffraction is not modelled separately; it is absorbed into the residual lens Gaussian.
* There is no depth-of-field variation across a tilted symbol, so defocus is uniform per frame.
* There is no HDR, multi-frame ISP fusion, or lens flare ghosts.
* Specular glare is additive and not polarization-aware.
* JPEG is modelled as DCT quantization of luma only, with no chroma subsampling or entropy coding. HEVC is not modelled.
* Background motion blur is limited to the symbol region.
