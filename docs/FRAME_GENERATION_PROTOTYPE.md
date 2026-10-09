# Frame generation prototype status

This branch starts the runtime presentation work behind the persisted FG toggle.

## Implemented in this prototype

- Retains the last fully composited NRLive output in a GPU-resident D3D12 texture.
- When FG is enabled and history is valid, emits a midpoint image by blending the previous output and the current post-FSR source on the GPU, then presents the real current image.
- Keeps the toggle off by default and invalidates history when the toggle changes.
- Does not use CPU readback for the generated image.

## Important limitation

This is **temporal frame blending**, not AMD FSR Frame Generation. It will ghost moving objects and UI because it does not warp pixels using motion vectors, resolve disocclusions, or use AMD's FSR FG runtime. It is an integration milestone to exercise GPU history, multiple presents, and toggle-off fallback before adding the official FSR FG API and presentation scheduler.

## Next implementation steps

1. Integrate the separate AMD FSR Frame Generation API/provider (not just the existing upscaler context).
2. Validate motion-vector direction, pixel units, frame delta, and synthetic-depth limitations.
3. Add a separate output-frame scheduler/pacing path and avoid waiting on the source thread.
4. Test visual quality, GPU timestamps, frame pacing, toggle transitions, resizing, and focus pause/resume on hardware.

AMD's FSR FG API requires its own context and per-frame preparation/dispatch; an upscaler dispatch is not a substitute. The current CI build can validate compilation and invariant tests only, not image quality or actual FG performance.
