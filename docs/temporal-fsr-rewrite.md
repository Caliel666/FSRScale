# Temporal FSR rewrite experiment

## Goal

Find the source of NRLive's perceived lag, smear, and uneven frame pacing by isolating capture, motion, FSR history, and presentation instead of changing all four at once. Keep the merged `main` branch as the control.

## What the Lossless Scaling addon actually does

The LS Addon Manager does not use Windows Graphics Capture to grab a separate game window. Its FSR addon replaces the NIS compute pass inside Lossless Scaling's own D3D11 pipeline. It reads the already-captured game frame through that pass's SRV, copies it into a shared D3D11 texture, signals a shared fence, and runs FSR 3.1 on its own D3D12 queue. It hands the finished image back to the NIS output without changing the original capture or presenting a second fullscreen window.

The bridge uses GPU-side fence waits, triple-buffered shared result slots, and skips work when the previous upscaling job is still in flight. The render/present thread does not CPU-wait for a late FSR result. This is a major architectural difference from NRLive's WGC -> D3D11 shared texture -> D3D12 FSR -> separate swapchain path.

Source references:
- https://github.com/Echo-Storm/ls-addon-manager/blob/main/addons/DLSS5NR01/docs/architecture.md
- https://github.com/Echo-Storm/ls-addon-manager/blob/main/addons/DLSS5NR01/src/addon/scaler11.cpp
- https://github.com/Echo-Storm/ls-addon-manager/blob/main/addons/DLSS5NR01/src/addon/bridge.cpp
- https://github.com/Echo-Storm/ls-addon-manager/blob/main/addons/DLSS5NR01/src/engine/sr_engine.h
- https://github.com/Echo-Storm/ls-addon-manager/blob/main/README.md

## Working hypotheses

1. **Presentation/capture timing:** WGC only yields frames when available, NRLive drains to the newest frame, and then presents through its own swapchain. This is latency-oriented but can produce uneven cadence compared with replacing a pass in the original present path.
2. **Temporal reconstruction without game jitter:** a captured game frame does not expose the game's jitter sequence or real depth. Feeding estimated motion to FSR cannot recreate the inputs of a native temporal-upscaler integration. At rest, FSR's history may trail or soften text.
3. **Motion estimator quality:** the current FastMv implementation is an experiment and must be measured independently; it should not be presumed better merely because it produces vectors.
4. **GPU queue contention:** any capture conversion, optical-flow estimate, FSR dispatch, and blit queued before present can delay the next output. The addon architecture explicitly skips stale work instead of blocking presentation.

## Experiment order

1. Preserve the merged implementation as the control; make all changes on this branch.
2. Record per-frame CPU time, capture age/sequence, motion-estimation GPU time, FSR GPU time, and present/fence wait time. Do not use a single FPS counter as the performance metric.
3. Establish a reproducible test matrix: static UI/text, slow camera pan, fast camera turn, particles/transparency, scene cut, 30/60/120 FPS input, and 1:1 vs upscale.
4. Compare `--mv amdof`, `--mv fast`, and a zero-motion baseline. Keep the exact same capture/presentation path for these comparisons.
5. Only after the baseline is measured, prototype an opt-in non-blocking DXGI-present tap / shared-texture path. Do not replace WGC by Desktop Duplication blindly: Desktop Duplication captures the monitor, not the game's internal swapchain, and risks capturing NRLive's own output.
6. Add regression tests for vector sign, static-frame zero motion, known translations, scene cuts, and GPU device removal. Keep the Windows CI build pinned to FidelityFX SDK 1.1.4.

## Acceptance criteria

- CI builds the Release x64 application and runs tests on every branch push and pull request.
- No test claims to validate visual quality merely because a shader compiles.
- GPU work does not CPU-block the output thread waiting for FSR completion.
- The capture path never samples NRLive's own output by accident.
- Performance claims are backed by repeatable frame traces, not subjective FPS alone.
