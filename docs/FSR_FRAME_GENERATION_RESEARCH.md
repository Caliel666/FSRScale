# FSR Frame Generation research plan (no runtime implementation)

## Current status

The overlay's **FG** button is a persisted research placeholder only. It changes its visual state and stores `[FrameGeneration] enabled=0|1` in `scaleconfig.ini`; it does not load a frame-generation DLL, dispatch interpolation, or change presentation cadence.

## Intended future ordering

1. Acquire a fresh captured game frame.
2. Run the current image-effects chain: OptiScaler's selected FFX backend and any ReShade effects observable in NRLive's final presentation path.
3. Apply NRLive's base-frame limiter to the source cadence.
4. Generate interpolated frames from the post-effect source frames.
5. Present generated and source frames using a dedicated presentation scheduler.

This is a design target, not a claim that every third-party hook currently executes at a known point. ReShade/OptiScaler integration must be traced on a real runtime; Present hooks can execute before or after application-level blits depending on injection target and API interception.

## Main technical constraints

- FSR 3 Frame Generation is not the same feature as FSR 3.1 Super Resolution. A super-resolution dispatch alone cannot enable frame generation.
- Frame generation needs a supported FG integration, frame pacing, swapchain/present handling, and suitable per-frame inputs.
- NRLive currently has synthetic depth and estimated motion vectors, not authoritative game depth, camera matrices, jitter, or UI masks. Optical-flow estimates may be sufficient for experiments but can produce disocclusion errors, UI artifacts, and ghosting.
- The existing FastMv and AMDOF outputs must be checked for vector direction, units, scaling, jitter convention, and validity before reuse. Dummy depth is not meaningful scene depth.
- OptiScaler's FFX API hook is an upscaler backend integration; do not assume it also provides an FG provider.
- Avoid CPU readback and blocking the source render/present thread. Use GPU-resident resources, explicit fences, and a small ring of output slots. If a slot is still busy, skip interpolation rather than stall the game.
- The base FPS limiter should cap source-frame cadence before interpolation; generated frames then run on a separate presentation schedule. Do not cap the final generated output to the same FPS, which would negate the intended multiplier.
- NRLive currently uses WGC capture → D3D11 copy/shared texture → D3D12 effects → output swapchain. A production design must establish whether it can interpolate the final post-ReShade surface or needs an earlier hook point.

## Research gates before implementation

1. Identify a redistributable AMD FG runtime/API and document licensing/runtime requirements.
2. Prototype a standalone D3D12 interpolation test using two known frames and validated motion vectors; verify vector conventions and occlusion behavior.
3. Measure GPU timestamps and end-to-end presentation intervals, including missed/deferred frames.
4. Compare source-only, interpolation-only, and combined latency; include a bypass hotkey.
5. Integrate only after it can fail open to the existing FSR/blit path without black frames or resource lifetime hazards.

## Acceptance criteria

- No CPU readback in the real-time interpolation path.
- No GPU wait on the game's source thread.
- A busy FG slot drops generated work rather than blocking.
- Toggle-off exactly restores current NRLive presentation behavior.
- The base-frame cap controls source cadence while FG controls generated presentation cadence independently.
- Test scrolling text, thin geometry, HUD elements, rapid camera movement, and disocclusions.

Until those gates pass, this branch intentionally ships no frame-generation runtime functionality.
