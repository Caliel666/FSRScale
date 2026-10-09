#pragma once

// Fast motion estimator shaders.  The design is deliberately GPU-first:
// a luma pyramid, coarse-to-fine 4x4 block search, a small vector median,
// then a five-candidate per-pixel resolve which also produces an FSR reactive
// mask.  The latter is important for captured game UI/effects: when the
// measured vector cannot explain the local change, FSR is told to prefer the
// current frame instead of dragging stale history across text.

namespace FastMvShaders {

inline constexpr const char* Common = R"(
cbuffer C : register(b0)
{
    uint2 size;
    uint2 grid;
    uint2 coarse;
    uint radius;
    uint flags;
    float strayWeight;
    float candidateBias;
    float fastMotion;
    float distrustThreshold;
    uint pad0;
    uint pad1;
};
)";

inline constexpr const char* Luma = R"(
Texture2D<float4> src : register(t0);
RWTexture2D<float> dst : register(u0);

[numthreads(8,8,1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= size.x || id.y >= size.y) return;
    float3 c = src.Load(int3(id.xy, 0)).rgb;
    dst[id.xy] = dot(c, float3(0.299, 0.587, 0.114));
}
)";

inline constexpr const char* Down = R"(
Texture2D<float> src : register(t0);
RWTexture2D<float> dst : register(u0);

[numthreads(8,8,1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= size.x || id.y >= size.y) return;
    int2 p = int2(id.xy) * 2;
    int2 last = int2(coarse) - 1;
    dst[id.xy] =
        0.25 * (
            src.Load(int3(min(p, last), 0)) +
            src.Load(int3(min(p + int2(1,0), last), 0)) +
            src.Load(int3(min(p + int2(0,1), last), 0)) +
            src.Load(int3(min(p + int2(1,1), last), 0)));
}
)";

// One invocation represents one 4x4 block.  At every pyramid level it searches
// only around the prediction from the level below.  The coarsest level has a
// wider search window, which captures camera pans without paying that cost at
// full resolution.  Half the 8x8 patch is sampled for candidate ranking.
inline constexpr const char* Search = R"(
Texture2D<float> cur : register(t0);
Texture2D<float> prev : register(t1);
Texture2D<float2> coarseFlow : register(t2);
RWTexture2D<float2> outFlow : register(u0);

float sadHalf(int2 origin, int2 d, int2 last)
{
    float s = 0.0;
    [unroll] for (int y = 0; y < 8; ++y)
    {
        [unroll] for (int x = 0; x < 8; x += 2)
        {
            int2 a = clamp(origin + int2(x + ((y & 1) != 0), y), int2(0,0), last);
            int2 b = clamp(a + d, int2(0,0), last);
            s += abs(cur.Load(int3(a,0)) - prev.Load(int3(b,0)));
        }
    }
    return s * (1.0 / 32.0);
}

float stray(float2 d, float2 prediction)
{
    return strayWeight * min(length(d - prediction), 2.0);
}

[numthreads(8,8,1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= grid.x || id.y >= grid.y) return;

    const int2 last = int2(size) - 1;
    const int2 origin = int2(id.xy) * 4 - 2;
    float2 prediction = float2(0,0);

    if ((flags & 1) != 0)
    {
        int2 cc = clamp((int2(id.xy) * 2 + 1) >> 2, int2(0,0), int2(coarse) - 1);
        prediction = coarseFlow.Load(int3(cc,0)) * 2.0;
    }

    int2 best = int2(round(prediction));
    float bestCost = sadHalf(origin, best, last) + stray(float2(best), prediction);

    // Always test no-motion explicitly. This is critical for static HUD/text:
    // a weak accidental match must never beat the exact zero-motion candidate.
    const float zeroCost = sadHalf(origin, int2(0,0), last);
    if (zeroCost < bestCost)
    {
        best = int2(0,0);
        bestCost = zeroCost;
    }

    const int r = (flags & 1) != 0 ? 2 : 4;
    [loop] for (int dy = -r; dy <= r; ++dy)
    {
        [loop] for (int dx = -r; dx <= r; ++dx)
        {
            int2 d = best + int2(dx,dy);
            float c = sadHalf(origin, d, last) + stray(float2(d), prediction);
            if (c < bestCost)
            {
                bestCost = c;
                best = d;
            }
        }
    }

    outFlow[id.xy] = float2(best);
}
)";

inline constexpr const char* Median = R"(
Texture2D<float2> src : register(t0);
RWTexture2D<float2> dst : register(u0);

[numthreads(8,8,1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= size.x || id.y >= size.y) return;

    float2 v[9];
    [unroll] for (int k = 0; k < 9; ++k)
    {
        int2 q = clamp(int2(id.xy) + int2(k % 3 - 1, k / 3 - 1),
                       int2(0,0), int2(size)-1);
        v[k] = src.Load(int3(q,0));
    }

    float bestScore = 1e30;
    float2 best = v[4];

    [unroll] for (int a = 0; a < 9; ++a)
    {
        float score = 0;
        [unroll] for (int b = 0; b < 9; ++b)
            score += abs(v[a].x-v[b].x) + abs(v[a].y-v[b].y);
        if (score < bestScore)
        {
            bestScore = score;
            best = v[a];
        }
    }

    dst[id.xy] = best;
}
)";



// Full-resolution resolve.  Each pixel considers zero motion, its block's
// vector, and the three adjacent block vectors.  The best local 3x3 match
// wins.  The same match error becomes a conservative reactive/distrust mask.
// A hard threshold on tiny vectors makes a completely still image exactly
// stationary, which is what keeps text from slowly walking through history.
inline constexpr const char* Pixel = R"(
Texture2D<float> cur : register(t0);
Texture2D<float> prev : register(t1);
Texture2D<float2> blockFlow : register(t2);
RWTexture2D<float2> outMotion : register(u0);
RWTexture2D<float> outReactive : register(u1);

SamplerState linearClamp : register(s0);

float patchCost(int2 p, float2 motion, int2 last, float2 invSize)
{
    float s = 0;
    [unroll] for (int y=-1; y<=1; ++y)
    {
        [unroll] for (int x=-1; x<=1; ++x)
        {
            int2 q = clamp(p + int2(x,y), int2(0,0), last);
            float2 uv = (float2(q) + 0.5 + motion) * invSize;
            s += abs(cur.Load(int3(q,0)) -
                     prev.SampleLevel(linearClamp, uv, 0));
        }
    }
    return s * (1.0/9.0);
}

[numthreads(8,8,1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= size.x || id.y >= size.y) return;
    if ((flags & 1) == 0)
    {
        outMotion[id.xy] = float2(0,0);
        outReactive[id.xy] = 0.0;
        return;
    }

    const int2 p = int2(id.xy);
    const int2 last = int2(size)-1;
    const int2 glast = int2(coarse)-1;
    const int2 cell = min(p >> 3, glast);
    const int sx = ((p.x & 7) < 4) ? -1 : 1;
    const int sy = ((p.y & 7) < 4) ? -1 : 1;

    float2 own = blockFlow.Load(int3(cell,0)) * 2.0;
    float2 right = blockFlow.Load(int3(clamp(cell + int2(sx,0), int2(0,0), glast),0)) * 2.0;
    float2 down = blockFlow.Load(int3(clamp(cell + int2(0,sy), int2(0,0), glast),0)) * 2.0;
    float2 diag = blockFlow.Load(int3(clamp(cell + int2(sx,sy), int2(0,0), glast),0)) * 2.0;

    const float2 invSize = 1.0 / float2(size);

    float2 candidates[5];
    candidates[0] = float2(0,0);
    candidates[1] = own;
    candidates[2] = right;
    candidates[3] = down;
    candidates[4] = diag;

    float bestCost = patchCost(p, candidates[0], last, invSize);
    float2 best = candidates[0];

    [unroll] for (int n=1; n<5; ++n)
    {
        float2 d = candidates[n];
        if (length(d) < 0.05) continue;

        float cost = patchCost(p, d, last, invSize) + candidateBias;
        if (cost < bestCost)
        {
            bestCost = cost;
            best = d;
        }
    }

    // If the local match is essentially exact, force zero instead of allowing
    // subpixel noise to move static pixels. This is intentionally asymmetric:
    // zero wins ties and near-ties.
    if (bestCost < 0.0025)
        best = float2(0,0);

    float reactive = saturate((bestCost - distrustThreshold) /
                              max(0.001, distrustThreshold * 2.0));

    // Fast camera motion is exactly where an estimated vector is least
    // trustworthy. Increase reactivity gradually instead of feeding a dubious
    // vector into FSR's history accumulator.
    if (fastMotion > 0.0)
        reactive = max(reactive,
                       saturate((length(best) - fastMotion) / fastMotion));

    outMotion[id.xy] = best;
    outReactive[id.xy] = min(reactive, 0.9);
}
)";

} // namespace FastMvShaders
