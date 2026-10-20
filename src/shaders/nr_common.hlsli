// The color bridge, 5c form: measured white point + invertible curve.
//
// The frame's linear values are first normalised by the measured white point (a smoothed
// log-average luminance the host reads back from the reduction pass), so mid-grey lands where the
// model expects regardless of the game's arbitrary exposure scale, and highlights keep gradation
// instead of being crushed into the top sliver of a fixed Reinhard. The curve itself stays
// Reinhard-on-normalised -- still exactly invertible, and encode/resolve run in the same frame
// with the same constants, so the round trip is lossless by construction.

float Luma(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

float3 EncodeDisplay(float3 linear_color, float white_point)
{
    float3 o = max(linear_color, 0.0) / max(white_point, 1e-4);
    float3 t = o / (1.0 + o);
    return pow(t, 1.0 / 2.2);
}

float3 DecodeDisplay(float3 display_color, float white_point)
{
    // 0.999 caps the inverse at ~1000x rather than infinity when the model writes pure white.
    float3 p = clamp(display_color, 0.0, 0.999);
    float3 t = pow(p, 2.2);
    return (t / (1.0 - t)) * max(white_point, 1e-4);
}

cbuffer Constants : register(b0)
{
    float g_transfer;     // how far the frame moves toward the model's answer; 0 = bypass
    float g_max_ratio;    // luminance guard: the model may move a pixel at most this far, either way
    float g_colour;       // 0 = keep the game's own hue, only brightness carries the model's verdict
    float g_white_point;  // measured (smoothed) white point x user scale; 1.0 until measured
    uint g_debug;         // 0 off, 1 proxy, 2 model answer, 3 difference x20
    uint g_width;
    uint g_height;
};

float3 Anchor(float3 o, float3 m)
{
    const float lo = Luma(o);
    const float lm = Luma(m);

    if (lm > 1e-8 && lo > 1e-8)
    {
        const float ratio = clamp(lm / lo, 1.0 / g_max_ratio, g_max_ratio);
        m *= (ratio * lo) / lm;  // luminance clamped to the guard, hue kept

        // Whether the model's colour arrives with its light.
        const float3 brightness_on_game_hue = o * (Luma(m) / lo);
        m = lerp(brightness_on_game_hue, m, g_colour);
    }
    else
    {
        m = o;  // true black stays true black; the model invents nothing there
    }

    // NaN guard: any non-finite lane falls back to the original.
    m = select(m == m, m, o);

    return lerp(o, m, g_transfer);
}
