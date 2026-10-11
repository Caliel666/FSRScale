# DLSSNR integration plan

This feature is developed on `feature/dlssnr-before-fsr`, separate from the capture/UI PRs.

## Required execution order

```
capture frame
  -> NRLive motion-vector pass (AMDOF or FastMV)
  -> DLSSNR-AMD (Windows Vulkan compute runtime; shared GPU resources/fences)
  -> FSR Super Resolution
  -> FSR Frame Generation preparation / presentation
  -> output
```

The NR stage must consume the same frame's motion-vector texture after it has transitioned to a shader-readable state. It must never substitute a second motion estimator when NRLive already produced vectors. The source image and NR result must remain GPU-resident; a per-frame CPU readback is not acceptable.

## Runtime and model licensing

- The DLSSNR-AMD Windows runtime and shader implementation are MIT-licensed; include the upstream MIT license with the portable package.
- The upstream project does not distribute NVIDIA's model weights. The package must not fetch or bundle proprietary weights. A user must provide a compatible `nvngx_dlssnr.dll` and use the upstream model-extraction tool to create `dlssnr.bin`.
- Include the upstream source/version and its license in the build artifact, and validate the runtime files before packaging.

## Settings surface

Persistent settings belong in `scaleconfig.ini` and include enable, model scale, style (Neutral/Natural/Cinematic), structure, intensity, skin structure/mask and temporal residual stabilizer controls. Settings are applied to the runtime at frame boundaries. The overlay toggle is an enable/disable switch, not a fake status indicator.

## Performance and correctness constraints

- No CPU readback or CPU wait in the steady-state frame loop.
- Reuse Vulkan device, shared allocations, pipelines, and history; rebuild only on resolution/model-scale changes.
- Run before FSR, at captured/render resolution.
- Pass NRLive's current motion vectors and reset history when capture, dimensions, or temporal continuity resets.
- The optional residual stabilizer is GPU-only and motion-reprojected. NRLive does not currently capture real game depth, so it cannot perform the reference stabilizer's depth/surface rejection and may ghost on disocclusions.
- If initialization or a frame fails, pass the original colour through unchanged and keep FSR/FG operational.
- Verify build/tests in GitHub Actions; GPU runtime quality/performance must still be measured on RDNA4 hardware.
