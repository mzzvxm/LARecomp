// MCLA GPU renderer -- the HDR scene's curve (hdr_tonemap.h).
//
// This file is not compiled with the renderer's own shaders. tools/build_hdr_variants.py (a local
// script of the shader pack's builders: it works on the game's translated shaders, which are
// never committed) pastes it into the HLSL of the game's tone map pixel shader and calls it in two
// places, then compiles the result for both backends into mcla_shaders_hdr.v1.pack:
//
//   Ld = HdrToneLd(Ls, White, g_HdrScene)   in place of the Reinhard curve's value (Ls is the
//                                           scene's luminance over the adapted luminance, White
//                                           the squared white of the game's curve),
//   oC0.xyz = HdrFinish(encoded colour)     in place of the shader's last assignment, so the
//                                           game's encode (its gamma, applied unclamped) writes
//                                           code values that may exceed 1 into a float target.
//
// g_HdrScene is the 16 bytes after the shared constants (constants.h HdrSceneConstants): x the
// knee, y the highlights factor, z the display's peak, both in the game's linear units (1 = its
// white), w unused. The algorithm is the fork's (patches/rexglue-hdr-scene-mcla.patch, its compute
// pass), whose look the owner signed off on: Reinhard extended up to the knee as the game has it,
// above it an exponential shoulder with the same value and slope times `highlights`, approaching
// the peak, never under the game's curve up to its white; with the peak at or below the game's white
// there is no room for a shoulder and the game's curve is clipped at its white.

#ifndef MCLA_GPU_HDR_TONEMAP_SHOULDER_HLSLI
#define MCLA_GPU_HDR_TONEMAP_SHOULDER_HLSLI

// Not NaN, by the bits (a compiler may fold x == x).
bool HdrNotNan(float value)
{
	return (asuint(value) & 0x7FFFFFFFu) <= 0x7F800000u;
}

// The tone map's luminance curve for `ls` (the scene's luminance over the adapted luminance,
// times the key) with the game's `white_squared`, shouldered above the knee.
float HdrToneLd(float ls, float white_squared, float4 hdr)
{
	const float knee = hdr.x;
	const float highlights = hdr.y;
	const float peak = hdr.z;
	// Not negative (the curve divides by 1 + Ls), not NaN, finite.
	ls = HdrNotNan(ls) ? clamp(ls, 0.0, 1.0e18) : 0.0;
	// A white of NaN or nothing: the comparison is false for NaN.
	const float white = white_squared > 1.0e-4 ? min(white_squared, 1.0e4) : 1.0e-4;
	// Reinhard extended, as the game computes it.
	float ld = ls * (1.0 + ls / white) / (1.0 + ls);
	const float b = 1.0 - knee;
	// Where the game's curve reaches the knee: Ls (1 + Ls / W) / (1 + Ls) = knee.
	const float ls_knee = (-b + sqrt(b * b + 4.0 * knee / white)) * 0.5 * white;
	if (ls > ls_knee)
	{
		// How far the shoulder may rise above the knee; no room above the game's white: its curve,
		// clipped at that white.
		const float span = max(peak - knee, 1.0e-3);
		if (span > b * 1.0001)
		{
			// The game's slope at the knee, times the highlights setting, and at least what reaches
			// the game's white at its white (where the game's curve bends up towards it, W < 1, a
			// shoulder that leaves the knee with the same slope would stay under it).
			float slope = ((1.0 + 2.0 * ls_knee / white) * (1.0 + ls_knee) -
			               (ls_knee + ls_knee * ls_knee / white)) /
			              ((1.0 + ls_knee) * (1.0 + ls_knee));
			slope *= highlights;
			const float white_ls = sqrt(white);
			if (white_ls > ls_knee)
			{
				slope = max(slope, -span * log(1.0 - b / span) / (white_ls - ls_knee));
			}
			const float shoulder =
				knee + span * (1.0 - exp2(-(ls - ls_knee) * slope / span * 1.44269504));
			// Below the game's white never under its curve (the max); above it the shoulder.
			ld = ls < white_ls ? max(shoulder, ld) : shoulder;
		}
		else
		{
			ld = min(ld, 1.0);
		}
	}
	return ld;
}

float HdrFiniteComponent(float value)
{
	// Not NaN, and inside what a float16 target holds; negative light is none.
	return HdrNotNan(value) ? clamp(value, 0.0, 65504.0) : 0.0;
}

// The encoded colour, unclamped above 1 but safe to write to a float16 target.
float3 HdrFinish(float3 code)
{
	return float3(HdrFiniteComponent(code.x), HdrFiniteComponent(code.y),
	              HdrFiniteComponent(code.z));
}

#endif  // MCLA_GPU_HDR_TONEMAP_SHOULDER_HLSLI
