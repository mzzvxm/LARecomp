// MCLA GPU renderer -- contact shadows, the sun's visibility over a short distance (ao.h).
//
// The game has no shadow for the small things in the world (a kerb, a wall's foot, a car's sills, a
// lamp post on the pavement): its sun shadows are four cascades whose texels are 1 to 25 cm and
// 4-tap filtered, and at 6 h, 18 h and in rain none at all. This pass finds the short shadows from
// the picture's own depth: for each pixel that faces the sun, a ray toward the sun is marched in
// view space over a short length (the push block says how many world units and how many steps) and
// every step is looked up in the linear depth image the occlusion's depth pass made (the chain's
// level 0, R32_FLOAT, at the AO's size, which is the render size: a render scale of 3 traces
// 3840x2160 pixels). A step is hidden when the surface at its pixel is nearer to the camera than the
// step, and not by more than the thickness (a wall far in front of the ray hides nothing: it is
// another object that only overlaps on screen). The result is 1 (open) to 0 (all hidden), R16_FLOAT,
// like the occlusion's visibility.
//
// A shadow of the sun is sharp: the sun covers half a degree, so an edge 0.4 m from its caster is
// 4 mm wide. The first version was soft for four reasons, all changed here; every number below is in
// contact_trace.h with a standalone check (contact_trace_test.cpp), which also holds a C++ twin of
// this march that the check runs over synthetic scenes:
//   1. The ray's end fade was 1 - along^2, so a shadow was darkest at its caster and lost a quarter
//      of its darkness half way along the ray: a gradient, not a shadow. Now the shadow is full
//      over the first `fade` share of the ray (push shape.x, 0.6) and only the last part fades, so
//      that a shadow cut off by the ray's end does not end in a hard line.
//   2. The depth tolerance (a bias of 2 cm + 0.4% of the distance, and a ramp as wide as the bias)
//      was set for 1280x720 and was the same at every render scale: at 4K an edge was 5 to 30
//      pixels wide. The tolerance is now a few footprints (the world size of one pixel of the depth
//      image at that distance: 2 z tan(fov/2) / height) plus a few millimetres, and so is the lift
//      of the ray's start off its surface (4 footprints along the normal). That lift is what keeps a
//      lit surface from shadowing itself: along the view ray it is 4 footprints times root(1 + s^2)
//      for a surface of slope s (depth change per pixel step over the footprint), and the error of
//      reading the depth at the centre of the pixel the step falls in is at most half a pixel times
//      s footprints, so it is always covered (contact_trace_test.cpp runs planes at every angle).
//   3. The denoise was two plane-only bilateral passes (ao_denoise_ps), which blur a shadow edge
//      across one surface. The contact term has its own single pass now (ao_denoise_ps, flag bit
//      25: AoContactFilter) whose weights also fall with the difference of the visibility, so noise
//      goes and an edge stays.
//   4. The steps were equally spaced and the start offset noise moved the whole ray a step: the
//      near part of the ray, where the contact is, is sampled finer now (the distance grows as the
//      square of the step number) and the step's ramp covers the depth change of a step, so the far
//      steps do not speckle.
//
// What it is not: a shadow of anything that is not on screen, and nothing thinner than the step
// (the start of the ray is moved per pixel by interleaved gradient noise plus the frame's R2 shift;
// there is no history for it, so the shift is 0 (a fixed pattern) unless a temporal upscaler
// averages the frames). On a car's own pixels that noise is a stipple a plate's shadow shows (THE VEHICLE
// MARCH'S STIPPLE below); the filter takes it out.
//
// The apply pass (ao_apply_ps) puts it on the picture: only on the sun's share of the light, so it
// cannot darken what the game already shadows (the game's mask says that) or the ambient.
//
// THE CAR'S OWN PIXELS (contact_trace.h "a car's own pixels"). A pixel that belongs to a vehicle (the depth pass's vehicle bit) marches a
// short, thin ray: at most kVehicleLength (0.3 m, not grown by a low sun), kVehicleThickness (6 cm) and kVehicleSteps steps. The thickness
// rule takes the surface in front of a step for a slab extruded behind it along the view ray, which a thin part of a car (a wing's 4 cm plate
// seen from above at a grazing angle, a 6 cm post) is not: with the sun low the lid's rays climbed onto the plate's pixels and the plate "hid"
// them, a broad black band across the trunk under the wing, and a post hid the ray of the pixels 0.1 to 0.3 m behind it, the streaks and blobs
// from the side. The game's own sun shadow map holds the player's car; what matters on a car's own surfaces is small (a mirror on its door).
// The shadows on the road (a car's sills, its tyres) are not a car's pixels: those march as they did.
//
// THE VEHICLE MARCH'S STIPPLE (contact_trace.h has the story, the measurements and the check). A car's own pixel marches 12 steps of a thin ray from a start the
// pixel's noise moves, and a thin plate over it (the wing over the trunk lid) is hit by some starts and missed by others: the pixel is 0 or 1 where the mean over
// the noise is a fraction, and without the car's map (CAR SHADOWS = OFF) nothing else is there, no history either, in the pause view or in play. The filter
// (ao_common.hlsli AoBilateralEdge, and the apply's folded copy) keeps a 0 and the 1 beside it apart, as it keeps a shadow's edge. A vehicle pixel that took steps
// therefore stores its visibility MARKED (v - kVehicleMarkOffset, a value no other pixel stores) and the filter weighs two marked pixels by position and depth alone,
// which averages the nine starts of a 3 x 3. The pixels the map covers take no steps and are not marked: the map's shadow keeps its crisp edge, and every other
// pixel is filtered as it was. Nothing else reads this image.
//
// THE MARK AND THE RAY TRACED TERM (flag bit 31, below; contact_trace.h "the mark and the ray traced term"). A vehicle pixel the traced sun ray holds in a shadow
// (traced_shadow at or above kRtUnmarkedShadow) has the ray's answer, exact and crisp, not the march's noise, and the filter must not average it with its neighbours:
// it is stored plain, as the pixels the car's map covers are, so that the traced shadow's edge keeps its width (the 3 x 3 of marked pixels would make it as wide as
// the map's was in the control of that check). The vehicle pixels the ray leaves lit still march and are marked, so the stipple of a false hit stays smoothed.
//
// THE GROUND UNDER A VEHICLE (the shadow starts as `Under` before the march). The game's sun shadow
// map holds the player's car only, so the ground under a traffic car is as lit as an open road, and the
// ray above finds the car only within `thickness` of itself: a pixel seen through the gap under a car
// has the car's rear face in front of it by up to the car's length, "far in front", which the thickness
// rule discards on purpose (it is meant for a wall that only overlaps the ray on the screen). A car is a
// solid, so for a ground pixel (a pixel that is not a vehicle's) the rule is different: looking up the column above a horizontal surface
// (the world's up, push lin.xyz) at two heights up to kUnderHeight, and up the sun's ray (to_sun) at the same heights, a probe that falls on
// a vehicle's pixel and lies behind its shell by more than the tolerance and less than kUnderReach is inside the car, and the ground is in its
// shadow when BOTH columns find it. The second column is what tells the ground under a car from the ground beyond it that the camera sees
// past the car's end (the false slab behind the rear tyre; the column alone calls both "under"); it is read only where the first column
// found a car. The player's car is left out by the host's cast distance (extra.w, at least 12 m): the game's own mask has it. The story and the
// numbers are in contact_trace.h ("the ground under a vehicle"), with a standalone check of the twin of this code.
//
// THE CAR'S SHADOW MAP (flag bit 28; contact_trace.h "the player's car's own sun shadow map" has the story, the numbers and the check). The
// game's sun shadow map holds the player's car at a low level of detail, and a march over the picture's depth cannot see a thin part's thickness.
// car_shadow_map.h draws the car again from the sun, depth only, at full detail, into a small square map fitted to the car; with a map in the
// frame (bit 28) a pixel whose view position falls inside it gets the car's own shadow from it, exact for the wing, the mirrors, the sills and
// the wheels on the car and on the road near it, and takes the DARKER of that and the march's. The apply pass multiplies by the sun's share
// where the game's own mask says lit (a vehicle's pixel takes the game's car sun image instead: THE GAME'S MASK IS NOT A CAR'S below), so the map never darkens what the game's mask shadows already on the world. The lookup is CarMapShadow below: a
// receiver-plane bias (the pixel's normal gives the depth's gradient per texel, and every tap compares at its own place on the plane), a
// normal offset, a blocker search and a filter whose half width is the sun's penumbra at the blocker's distance (blocker distance times
// kCarMapSunRadius, between kCarMapMinFilterTexels and kCarMapMaxFilterTexels texels), the border fading out. A vehicle pixel whose four
// centre texels hold a part of the car at the pixel's depth or in front of it is covered: the map is the truth about the car's own parts there and
// the short vehicle march takes no steps (flag bit 30 keeps it, for the comparison). Without bit 28, and for a pixel the map does not reach,
// the pass is as it was. Flag bit 29 is the debug view: the pass writes the map's term alone (1 minus the shadow, the facing weight included).
//
// THE RAY TRACED TERM (flag bit 31; car_rt_ao.h, car_rt_ao_rules.h). Where the device can trace rays against the player's car, a compute pass
// (car_rt_ao_cs.hlsl) casts a ray toward the sun from every pixel near the car and its shadow, against the car's triangles, and leaves an image
// (R8G8B8A8_UNORM, the size of the occlusion's traced image: the picture's, or half of it where the occlusion is traced at half size) whose g channel is
// 1 where the sun reaches the pixel and 0 where the car's own parts are in its way. With bit 31 a pixel takes the DARKER of its shadow so far (the
// march's, the car's shadow map's) and 1 - g, read with the linear sampler at the pixel's own place: exact for the wing's plate on the lid, a mirror on
// the door, the car on the road, with no map resolution, bias or peter-panning (contact_trace.h "the player's car's own sun shadow map" is the same
// kind of term). The apply pass still puts it on the sun's share where the game's own mask says lit. Flag bit 25 is the debug view of this term
// alone (mcla_gpu_contact_shadows_view 7; bit 25 is the contact filter's in the passes that filter, not here). Without bit 31 the pass is as it was.
//
// Push block (ao_common.hlsli AoPush), this pass's fields:
//   slots.z    the linear depth image (bindless slot); its lowest bit is the vehicle bit; slots.x, the statistics picture only: the mask's slot;
//              with flag bit 31 the traced image's slot (the statistics picture's pass has no bit 31)
//   slots.y    the normal image's bindless slot in the high 16 bits (0: none, the depth's normal; ao_common.hlsli AoNormalAt; the low bits are the host sample, not read here)
//   slots.w    the car's shadow map (bit 28): its bindless slot, one float channel, the depth from the sun (0 nearest, 1 where no part of the car is)
//   slots2.x   the car's shadow map (bit 28): the slot of its matrix, a 4 x 1 R32G32B32A32_FLOAT texture, texels 0 to 2 the rows that take a
//              view position to (u, v, depth) (car_shadow_map.h), texel 3 (texel_m, depth_m, size, the world's shadow filter: mode * 256 + the
//              sun in sixteenths; mode 0 is the map's own ring, 1 the soft disk, 2 pcss; contact_trace.h "THE FILTER FOLLOWS THE WORLD'S")
//   slots2.y   the frame number (the noise's shift); 0 while no temporal upscaler runs
//   slots2.z   flags: bit 24 the skip under the game's own shadow (THE SKIP below); bit 27 a draw that is not the march: the skip's
//              block pass, or with bit 26 the statistics picture (THE SKIP below); bits 28 to 30 the car's shadow map (above: a map is
//              bound, the debug view of its term alone, the vehicle march kept on the pixels it covers); the other bits are not read here
//   slots2.w   for the skip only: the block image's bindless slot (the block pass: the mask's)
//   sizes.zw   the image's size (this pass's target and the depth image are the same size); sizes.xy, for the skip only:
//              the mask's size over the image's size over kSkipBlock, per axis
//   lens.xy    tan of the half field of view; lens.z the ray's length in world units, lens.w the
//              thickness (the farthest a surface may be in front of a step and still hide it)
//   shape.x    the share of the ray over which the shadow stays full before it fades out (0.6)
//   shape.y    the number of steps (at most: see shape.z)
//   shape.z    the adaptive step count's end gap in pixels (THE ADAPTIVE STEP COUNT below; 0: every step)
//   light.xyz  the direction to the sun in view space (x right, y up, z forward; unit length; the under term's second column follows it)
//   extra.y    the distance beyond which nothing is computed (the caller's contact fade end)
//   lin.xyz    the world's up in view space (unit length; zero: no under-a-vehicle term), lin.w how
//              strongly that term shadows (0 to 1; 0: off, the pass is as it was)
//              (the contact pass does not read the depth rows lin otherwise holds)
//   extra.w    the distance in metres from the camera within which the game draws cars into the sun shadow
//              map (the player's car always, traffic with TRAFFIC SHADOWS: traffic_shadow.h, contact_trace.h UnderCastDistance, at
//              least 12 m): the under-a-vehicle term leaves out a car whose shell is nearer than that, which has its own shadow,
//              and keeps the farther ones
//   shape.w, extra.x, extra.z   for the skip only: the block pass's limit, and one over the block image's (the block pass: the
//                               mask's) width and height
//
// THE ADAPTIVE STEP COUNT (mcla_gpu_contact_shadows_fast; shape.z, contact_trace.h AdaptiveSteps). The steps are the
// squared spacing of the setting's count n, so the last two are 2 L / n pixels apart for a ray of L pixels on the
// screen. A ray that is short on the screen (a far pixel, a small picture) reads the same pixels again and again: the
// count is ceil(2 L / shape.z), between kAdaptMinSteps and the setting's (shape.z 8: never a coarser end than the full
// march has on a 64 pixel ray; contact_trace.h has the numbers and the check). L is the ray's start and end projected
// into the depth image (an end behind the near plane: every step). The host leaves it off (shape.z 0) above
// kAdaptMaxHeight lines of picture, where nearly every ray is long enough to keep all its steps (at 3840x2160 15.84 of 16)
// and the count's arithmetic (38 instructions a pixel, half a step's 79) costs more than the steps it saves.
//
// THE SKIP UNDER THE GAME'S OWN SHADOW (mcla_gpu_contact_shadows_fast; contact_trace.h "the skip under the game's own
// shadow" has the proof and the check). The apply pass uses the march's image only where the contact shadows can darken: the
// weight strength * sun_share * fade is 0 where the game's sun shadow mask is 0, and the 3x3 filter the apply does over this
// image reads a pixel's 8 neighbours only where that weight is above its own threshold. So a pixel returns 1 at once when
// every mask texel that the apply's bilinear sample can reach for the pixel or any of its 8 neighbours is at most `limit`,
// the largest stored value at which the weight is still under kSkipWeight (SkipLimit, made by the host from the frame's
// ambient and sun lumas, the strength and the gain; for an 8-bit mask that is 0, apart from a very dim sun against the
// ambient). Exact: the factor of every pixel is the same as without the skip, to the bit; and a test that skips fewer
// pixels than that is exact too, which is what lets the test be cheap.
// THE TEST'S COST. The march is bound by its arithmetic (1594 DXIL instructions a pixel that marches 16 steps, 161 for one that
// faces away from the sun), not by its depth reads, so the test is few instructions, and the first of them decides for the
// pixels in the sun. The host makes the limit, the scales and the reciprocals (no GetDimensions, no division here), and a
// BLOCK PASS (this shader, flag bit 27, at 1/4 of the mask's size: a small draw) makes an image of 4 x 4 texel blocks of
// the mask: 1 where any texel of the block is above the limit. The range of one pixel's footprint is at most 5 texels a side
// where the mask is the picture's size, so it touches 1 or 2 blocks a side: (1) the block the pixel's own centre falls in is
// part of it, so a 1 there ends the test with one load; only a pixel whose centre block is 0 goes on to (2) one gather of
// the 2 x 2 blocks from the first block of the range (more, one gather for each 2 blocks a side, for a mask wider than
// the picture), the first 1 ends the test. A pixel within a few texels of the sun is marched that the exact range would
// skip: fewer pixels skipped is exact too. The first version made the limit, the sizes' ratio and the range from the mask's
// GetDimensions in about 90 instructions for every pixel and read up to 25 texels one by one.
//   flags bit 24 (kSkipFlag)   the skip is on (the host sets it only with the filter inside the apply, a mask in the frame,
//                              a limit that is proven (SkipLimit >= 0), a mask not much larger than the picture and no
//                              debug picture); slots2.w the block image's bindless slot (R16_FLOAT, 0 or 1);
//   sizes.xy                   the mask's size over the image's size over 4, per axis (the host's float division);
//   extra.x, extra.z           one over the block image's width and height (the gather's coordinates).
//   flags bit 27 (kSideFlag)  this draw is the block pass (alone), into the block image: slots2.w the mask's bindless slot (R of the
//                              8-bit image), shape.w the limit (contact_trace.h SkipLimit), extra.x, extra.z one over the mask's
//                              width and height.
//
// THE SKIP STATISTICS (mcla_gpu_contact_skip_stat, flag bit 26 kStatFlag together with bit 27; ao.cpp RecordContactStat). A second pass of this shader into
// a scratch image on a few frames, which does the march's early parts (the sky test, the skip, the normal, the facing test: SkipStat)
// and writes what the pixel did in place of its visibility: -1 skipped, 2 the sky or past the contact fade, v (0 to 1) a pixel that
// marches and 3 + v one that faces away from the sun, v the stored mask value at the pixel's own centre texel (the host's
// histogram of v shows whether the game's shade stores 0, which the skip needs). Push: slots.x the mask's bindless slot, the rest as
// the march's. The host counts the picture on the CPU. A pass of its own, so the march pays nothing for it.
//
// THE GAME'S MASK IS NOT A CAR'S (contact_trace.h has the measurements; ao_apply_ps.hlsl CarAwareSunVisibility). The game collects its sun
// shadow mask before the cars are drawn, so at a car's pixel it holds the world behind the car (the iron fence's bars on the R34's trunk lid: the
// comb under the wing), and a car's own shader does not use it. The apply takes a vehicle's sun visibility from the game's car sun image (1 where the frame has none), never from the mask, so the skip above, which
// is exact only where the apply's weight is 0 where the mask is, is not allowed on a vehicle's pixel or on any of its 8 neighbours (VehicleInReach, asked
// only for a pixel the mask test would skip): the image can be lit where the mask is 0.
//
// The constants below are contact_trace.h's: change them there first.

#include "ao_common.hlsli"
#include "host_pass_samplers.hlsli"

// The depth tolerance, as a floor in metres plus footprints (the world size of one pixel of the
// depth image where the step is): a step counts as hidden once the surface at its pixel is nearer
// than the step by more than kEpsFloor + kEpsPixels footprints, fully so after kRampFloor +
// kRampPixels footprints more.
static const float kEpsFloor = 0.004f;
static const float kEpsPixels = 1.5f;
static const float kRampFloor = 0.004f;
static const float kRampPixels = 2.0f;
// Plus this share of the depth a step changes by along the sun's direction, so that the steps of a
// ray that is not parallel to the picture's plane run together instead of speckling.
static const float kRampStepShare = 0.5f;
// The lift of the ray's start off the surface it starts on, along the normal.
static const float kLiftFloor = 0.006f;
static const float kLiftPixels = 4.0f;
// A surface this nearly edge-on to the sun gets no contact shadow (its sun share is about 0 too).
static const float kSunFacingLow = 0.03f;
static const float kSunFacingHigh = 0.15f;
// The fade's start is kept below 1 (a fade of no width).
static const float kFadeStartMax = 0.98f;
// A step nearer to the camera than this is not looked up (behind the lens).
static const float kNearZ = 0.05f;
// The fewest steps the adaptive count goes down to (as a float: contact_trace.h's test compares the constant).
static const float kAdaptMinSteps = 4.0f;
// The skip under the game's own shadow: the weight below which the apply's contact term is left out (half the apply's
// kFoldedFilterSkip: the host's SkipLimit uses it), the margin added to the mask texel range (the sampler's fixed-point
// coordinates), the widest span of texels the host allows (a mask much larger than the image: no skip), and the flags.
static const float kSkipWeight = 0.001f;
static const float kSkipTexelMargin = 0.015625f;
static const float kSkipMaxTexels = 8.0f;
static const uint kSkipFlag = 1u << 24;
static const uint kStatFlag = 1u << 26;
// What a pixel writes in the statistics picture (contact_trace.h kStat*).
static const float kStatSkipped = -1.0f;
static const float kStatFar = 2.0f;
static const float kStatMarch = 0.0f;
static const float kStatBack = 3.0f;
static const uint kSideFlag = 1u << 27;
// The block pass's block: texels a side (contact_trace.h kSkipBlock).
static const float kSkipBlock = 4.0f;

// The player's car's own sun shadow map (header comment, contact_trace.h kCarMap*): the flags, and the numbers of the lookup.
static const uint kCarMapFlag = 1u << 28;
static const uint kCarMapViewFlag = 1u << 29;
static const uint kCarMapKeepMarchFlag = 1u << 30;
// The ray traced term (header comment; car_rt_ao_rules.h kContactFlag and kContactViewFlag).
static const uint kRtContactFlag = 1u << 31;
static const uint kRtContactViewFlag = 1u << 25;
// THE MARK AND THE RAY TRACED TERM (header comment; contact_trace.h kRtUnmarkedShadow): a vehicle pixel whose traced sun term is at least this much shadow is stored plain.
static const float kRtUnmarkedShadow = 0.5f;
static const float kCarMapSunRadius = 0.0044f;
static const float kCarMapMinFilterTexels = 0.75f;
static const float kCarMapMaxFilterTexels = 6.0f;
static const float kCarMapSearchMetres = 0.03f;
static const float kCarMapSearchMinTexels = 1.5f;
static const float kCarMapNormalOffsetTexels = 0.5f;
static const float kCarMapBiasFloor = 0.01f;
static const float kCarMapBiasTexels = 0.25f;
static const float kCarMapFootprintBias = 2.0f;
static const float kCarMapProducerSlope = 2.0f;
static const float kCarMapMaxTan = 8.0f;
static const float kCarMapEdgeTexels = 4.0f;
static const float kCarMapEmpty = 0.9999f;
// The world's filter on the car's map (contact_trace.h "THE FILTER FOLLOWS THE WORLD'S"): the matrix texture's last component is mode * 256 + the sun in sixteenths.
static const uint kCarMapFilterPcss = 2u;
static const float kCarMapSoftRadiusMetres = 0.03f;
static const float kCarMapSunTan = 0.0047123890f;
static const float kCarMapFilterMaxRadii = 2.0f;
static const int kCarMapFilterTaps = 16;
static const int kCarMapSearchTaps = 8;
static const float kCarMapFixedAngle = 2.399963f;
static const float kCarMapGoldenCos = -0.7373688780783197f;
static const float kCarMapGoldenSin = 0.6754902942615238f;

// The ground under a vehicle (header comment). Two columns of two probes, one straight up from the pixel and one along the
// sun's ray (the same heights up it, kUnderHeight times the share times a jitter of kUnderJitterLow + kUnderJitterSpan
// noise): a probe counts when the pixel it falls on is a vehicle's and the point is behind its shell by more than
// kUnderEpsFloor + kUnderEpsPixels footprints (fully so at twice that) and less than kUnderReach (then fading out over a
// quarter of it). The ground is under the car when both columns find it. The surface must be horizontal: its normal
// against the world's up from kUnderUpLow to kUnderUpHigh.
static const float kUnderHeight = 0.9f;
static const float kUnderStepHigh = 1.0f;
static const float kUnderStepLow = 0.4f;
static const float kUnderJitterLow = 0.85f;
static const float kUnderJitterSpan = 0.3f;
static const float kUnderReach = 6.0f;
static const float kUnderEpsFloor = 0.05f;
static const float kUnderEpsPixels = 3.0f;
static const float kUnderUpLow = 0.75f;
static const float kUnderUpHigh = 0.9f;
static const float kUnderSunSinMin = 0.25f;
// A car that casts into the sun shadow map (extra.w) is left out of the term by its shell's distance from
// the camera, fading in over this many metres either side of that distance (contact_trace.h).
static const float kUnderCastBand = 3.0f;
// A pixel of a vehicle marches a short, thin ray of at most this many steps (contact_trace.h kVehicleReceiver*).
static const float kVehicleLength = 0.3f;
static const float kVehicleThickness = 0.06f;
static const float kVehicleSteps = 12.0f;

// The weight of one probe of the term, 0 to 1 (contact_trace.h UnderProbe): `p` is the point in view space, `size` and
// `isize` the depth image's size.
float UnderProbe(uint depth_slot, float3 p, float2 size, int2 isize, float footprint_unit, float cast_distance) {
  if (p.z <= kNearZ) {
    return 0.0f;
  }
  const float2 uv = float2(0.5f + 0.5f * p.x / (p.z * g_push.lens.x),
                           0.5f - 0.5f * p.y / (p.z * g_push.lens.y));
  if (any(uv < 0.0f) || any(uv >= 1.0f)) {
    return 0.0f;
  }
  const float td = AoLoadLinear(depth_slot, min(int2(uv * size), isize - 1));
  if (!AoIsVehicle(td)) {
    return 0.0f;
  }
  const float eps = kUnderEpsFloor + kUnderEpsPixels * footprint_unit * p.z;
  const float dz = p.z - td;
  float weight = saturate((dz - eps) / eps) *
                 (1.0f - saturate((dz - kUnderReach) / (0.25f * kUnderReach)));
  if (cast_distance > 0.0f) {
    // The shell's distance from the camera along this pixel's ray (td is its depth, the ray's z).
    const float2 slope = p.xy / p.z;
    const float shell = td * sqrt(1.0f + dot(slope, slope));
    weight *= smoothstep(cast_distance - kUnderCastBand, cast_distance + kUnderCastBand, shell);
  }
  return weight;
}

// How much of the ground under a vehicle this pixel is, 0 to 1 (contact_trace.h UnderVehicle). `tc` is the
// pixel's own level-0 depth (with the vehicle bit), `pc` its view position, `normal` its view-space normal
// facing the camera, `to_sun` the direction to the sun, `size` and `isize` the depth image's size.
float Under(uint depth_slot, float tc, float3 pc, float3 normal, float3 to_sun, float2 size, int2 isize,
            float footprint_unit, float noise) {
  const float3 up = g_push.lin.xyz;
  const float strength = saturate(g_push.lin.w);
  const float cast_distance = g_push.extra.w;
  if (!(strength > 0.0f) || dot(up, up) < 0.5f || AoIsVehicle(tc)) {
    return 0.0f;
  }
  const float upness = smoothstep(kUnderUpLow, kUnderUpHigh, dot(normal, up));
  if (!(upness > 0.0f)) {
    return 0.0f;
  }
  const float jitter = kUnderJitterLow + kUnderJitterSpan * noise;
  const float high = kUnderHeight * kUnderStepHigh * jitter;
  const float low = kUnderHeight * kUnderStepLow * jitter;
  float hit = max(UnderProbe(depth_slot, pc + up * high, size, isize, footprint_unit, cast_distance),
                  UnderProbe(depth_slot, pc + up * low, size, isize, footprint_unit, cast_distance));
  if (hit > 0.0f) {
    // The same heights up the sun's ray (a point h above the ground is h over the sun's height along it). A sun at or below the horizon
    // reaches no ground under a car.
    const float sun_up = dot(to_sun, up);
    if (!(sun_up > 0.0f)) {
      return 0.0f;
    }
    const float along = 1.0f / max(sun_up, kUnderSunSinMin);
    const float sun = max(UnderProbe(depth_slot, pc + to_sun * (high * along), size, isize, footprint_unit, cast_distance),
                          UnderProbe(depth_slot, pc + to_sun * (low * along), size, isize, footprint_unit, cast_distance));
    hit = min(hit, sun);
  }
  return hit * upness * strength;
}

// One gather of the car's map at the texel position `tex` (centres at +0.5; contact_trace.h CarMapGather): the four texels in the gather's
// order (x (i0, j1), y (i1, j1), z (i1, j0), w (i0, j0)). CHECK: that order is the documented one of both backends (not seen here), the map is a
// D32 float + S8 target whose depth plane is read as one float channel with the point sampler (debug view 5 shows a map shifted by a texel, or
// all black, if either is wrong). `occupied` 1 where the texel holds a part of the car, `over` the receiver's depth at
// the texel (metres along the sun's ray, on the receiver's plane: `receiver_m` at the texel `centre`, `gradient` metres per texel along u, v)
// minus the stored one, `blocked` occupied and over the bias, `weight` the bilinear weights of a sample at `tex`.
void CarMapGather(uint slot, float2 tex, float2 centre, float receiver_m, float2 gradient, float depth_m, float inv_size, float bias,
                  out float4 occupied, out float4 blocked, out float4 over, out float4 weight) {
  const float2 base = floor(tex - 0.5f);
  const float2 f = tex - 0.5f - base;
  const float4 s = g_textures[slot].GatherRed(g_samplers[kSamplerPointClamp], (base + 1.0f) * inv_size);
  const float4 dx = float4(base.x, base.x + 1.0f, base.x + 1.0f, base.x) + 0.5f - centre.x;
  const float4 dy = float4(base.y + 1.0f, base.y + 1.0f, base.y, base.y) + 0.5f - centre.y;
  over = receiver_m + gradient.x * dx + gradient.y * dy - s * depth_m;
  occupied = (float4)(s < kCarMapEmpty);
  blocked = (float4)(over > bias) * occupied;
  weight = float4((1.0f - f.x) * f.y, f.x * f.y, f.x * (1.0f - f.y), (1.0f - f.x) * (1.0f - f.y));
}

// The car's shadow at a pixel, 0 (lit) to 1 (in the car's shadow), before the facing weight (contact_trace.h CarMapShadow is the twin: the
// same operations in the same order). `covered` says whether the four texels at the pixel hold a part of the car. 0 outside the map.
float CarMapShadow(float3 pc, float3 normal, float footprint, float noise, out bool covered) {
  covered = false;
  const uint matrix_slot = g_push.slots2.x;
  const float4 row_u = g_textures[matrix_slot].Load(int3(0, 0, 0));
  const float4 row_v = g_textures[matrix_slot].Load(int3(1, 0, 0));
  const float4 row_d = g_textures[matrix_slot].Load(int3(2, 0, 0));
  const float4 info = g_textures[matrix_slot].Load(int3(3, 0, 0));  // (texel_m, depth_m, size, 1)
  const float texel_m = info.x;
  const float depth_m = info.y;
  const float size = info.z;
  const uint filter_code = uint(info.w + 0.5f);
  const uint filter_mode = filter_code >> 8;
  const float sun_times = float(filter_code & 255u) * (1.0f / 16.0f);
  if (!(texel_m > 0.0f && depth_m > 0.0f && size >= 2.0f)) {
    return 0.0f;
  }
  // The map's axes in view space, unit (the rows are 1 / side long): u and v across the map, the depth away from the sun.
  const float side = size * texel_m;
  const float3 axis_u = row_u.xyz * side;
  const float3 axis_v = row_v.xyz * side;
  const float3 axis_d = row_d.xyz * depth_m;
  const float facing_map = max(-dot(normal, axis_d), 1.0f / kCarMapMaxTan);
  const float sin_t = sqrt(saturate(1.0f - facing_map * facing_map));
  const float3 p = pc + normal * (kCarMapNormalOffsetTexels * texel_m * sin_t);
  const float u = dot(p, row_u.xyz) + row_u.w;
  const float v = dot(p, row_v.xyz) + row_v.w;
  const float edge = min(min(u, 1.0f - u), min(v, 1.0f - v)) * size;  // texels to the border
  if (edge <= 0.0f) {
    return 0.0f;
  }
  const float receiver_m = (dot(p, row_d.xyz) + row_d.w) * depth_m;
  // The receiver's plane: metres of depth per texel along u and along v, held to kCarMapMaxTan texels per texel.
  const float limit = kCarMapMaxTan * texel_m;
  const float2 gradient = clamp(texel_m * float2(dot(normal, axis_u), dot(normal, axis_v)) / facing_map, -limit, limit);
  const float bias = kCarMapBiasFloor + kCarMapBiasTexels * texel_m + kCarMapFootprintBias * footprint;
  // The producer's slope scaled bias has pushed the map's own surface this much deeper: a pixel on it is nearer the sun than the map by that much.
  const float cover = bias + kCarMapProducerSlope * max(abs(gradient.x), abs(gradient.y));
  const uint slot = g_push.slots.w;
  const float inv_size = 1.0f / size;
  const float2 centre = float2(u, v) * size;
  float sa, ca;
  sincos(noise * 1.57079632679f, sa, ca);
  const float2 d0 = float2(ca, sa);
  const float2 d1 = float2(-sa, ca);
  float4 occupied, blocked, over, weight;
  // The blocker search: the centre, then a ring at the search radius.
  float blocker_sum = 0.0f;
  float blocker_count = 0.0f;
  CarMapGather(slot, centre, centre, receiver_m, gradient, depth_m, inv_size, bias, occupied, blocked, over, weight);
  covered = dot(occupied * (float4)(over > -cover), 1.0f) > 0.5f;  // a part of the car at the pixel's depth or in front of it
  blocker_sum += dot(blocked, over);
  blocker_count += dot(blocked, 1.0f);
  float total = dot(blocked, weight);  // the centre's share; the filter's first tap
  if (filter_mode != 0u) {
    // The world's filter (soft: the disk of a fixed radius; pcss: at the penumbra's width): the search over the disk's reach (the centre is its first
    // tap), then 16 taps on a Vogel disk, every one a bilinear gather. The disk keeps its angle unless a temporal upscaler runs (the push's frame is not 0).
    const float soft = kCarMapSoftRadiusMetres / texel_m;
    const float reach = filter_mode == kCarMapFilterPcss ? kCarMapFilterMaxRadii * soft : soft;
    float2 direction;
    sincos((g_push.slots2.y != 0u) ? noise * 6.28318530718f : kCarMapFixedAngle, direction.y, direction.x);
    [unroll] for (int j = 0; j < kCarMapSearchTaps; ++j) {
      const float at = (float(j) + 0.5f) * (1.0f / float(kCarMapSearchTaps));
      CarMapGather(slot, centre + direction * (sqrt(at) * reach), centre, receiver_m, gradient, depth_m, inv_size, bias, occupied, blocked, over, weight);
      blocker_sum += dot(blocked, over);
      blocker_count += dot(blocked, 1.0f);
      direction = float2(direction.x * kCarMapGoldenCos - direction.y * kCarMapGoldenSin,
                         direction.x * kCarMapGoldenSin + direction.y * kCarMapGoldenCos);
    }
    if (blocker_count < 0.5f) {
      return 0.0f;
    }
    float radius = soft;
    if (filter_mode == kCarMapFilterPcss) {
      radius = clamp(blocker_sum / blocker_count * kCarMapSunTan * sun_times / texel_m, kCarMapMinFilterTexels, reach);
    }
    float sum = 0.0f;
    float weights = 0.0f;
    [loop] for (int i = 0; i < kCarMapFilterTaps; ++i) {
      const float at = (float(i) + 0.5f) * (1.0f / float(kCarMapFilterTaps));
      const float w = 1.0f - 0.5f * at;
      CarMapGather(slot, centre + direction * (sqrt(at) * radius), centre, receiver_m, gradient, depth_m, inv_size, bias, occupied, blocked, over, weight);
      sum += w * dot(blocked, weight);
      weights += w;
      direction = float2(direction.x * kCarMapGoldenCos - direction.y * kCarMapGoldenSin,
                         direction.x * kCarMapGoldenSin + direction.y * kCarMapGoldenCos);
    }
    return saturate(sum / weights) * saturate(edge / kCarMapEdgeTexels);
  }
  const float r0 = clamp(kCarMapSearchMetres / texel_m, kCarMapSearchMinTexels, kCarMapMaxFilterTexels);
  const float2 ring[4] = {d0, d1, -d0, -d1};
  [unroll] for (int i = 0; i < 4; ++i) {
    CarMapGather(slot, centre + r0 * ring[i], centre, receiver_m, gradient, depth_m, inv_size, bias, occupied, blocked, over, weight);
    blocker_sum += dot(blocked, over);
    blocker_count += dot(blocked, 1.0f);
  }
  if (blocker_count < 0.5f) {
    return 0.0f;  // nothing in the way within the penumbra of the farthest blocker the map can hold
  }
  // The filter: the penumbra's half width at the mean blocker distance, a ring turned an eighth against the search's.
  const float r1 = clamp(blocker_sum / blocker_count * kCarMapSunRadius / texel_m, kCarMapMinFilterTexels, kCarMapMaxFilterTexels);
  const float2 a45 = (d0 + d1) * 0.70710678118f;
  const float2 b45 = float2(-a45.y, a45.x);
  const float2 ring2[4] = {a45, b45, -a45, -b45};
  [unroll] for (int j = 0; j < 4; ++j) {
    CarMapGather(slot, centre + r1 * ring2[j], centre, receiver_m, gradient, depth_m, inv_size, bias, occupied, blocked, over, weight);
    total += dot(blocked, weight);
  }
  return saturate(total * 0.2f) * saturate(edge / kCarMapEdgeTexels);
}

// True when the pixel or one of its 8 neighbours is a vehicle's (contact_trace.h VehicleInReach: the skip is not allowed there). The apply takes
// a vehicle's sun visibility as 1 whatever the game's mask holds at it (THE GAME'S MASK IS NOT A CAR'S above), so the skip's proof (a weight of 0
// where the mask is 0) does not hold for it, and the filter of a vehicle's pixel reads the 8 neighbours. Asked only after the mask test said
// skip, so a pixel in the sun pays nothing. `tc` is the pixel's own level-0 depth (with its bit); `isize` the image's size.
bool VehicleInReach(uint depth_slot, int2 pixel, int2 isize, float tc) {
  if (AoIsVehicle(tc)) {
    return true;
  }
  [unroll] for (int dy = -1; dy <= 1; ++dy) {
    [unroll] for (int dx = -1; dx <= 1; ++dx) {
      if ((dx != 0 || dy != 0) &&
          AoIsVehicle(AoLoadLinear(depth_slot, clamp(pixel + int2(dx, dy), int2(0, 0), isize - 1)))) {
        return true;
      }
    }
  }
  return false;
}

// The skip's block pass: the block image's texel is 1 where any of the 4 x 4 mask texels at it (clamped into the mask) is above
// the limit, else 0. Four gathers, at the corners between the block's 2 x 2 groups: a texel's coordinate c, in texels, is the
// corner between texels c - 1 and c, and the clamp to edge repeats the mask's border where a block hangs over it.
float4 SkipBlocks(float2 position) {
  const uint slot = g_push.slots2.w;
  const float2 inv_mask = float2(g_push.extra.x, g_push.extra.z);
  const float2 corner = floor(position) * kSkipBlock + 1.0f;
  const float4 a = g_textures[slot].GatherRed(g_samplers[kSamplerLinearClamp], corner * inv_mask);
  const float4 b = g_textures[slot].GatherRed(g_samplers[kSamplerLinearClamp], (corner + float2(2.0f, 0.0f)) * inv_mask);
  const float4 c = g_textures[slot].GatherRed(g_samplers[kSamplerLinearClamp], (corner + float2(0.0f, 2.0f)) * inv_mask);
  const float4 d = g_textures[slot].GatherRed(g_samplers[kSamplerLinearClamp], (corner + float2(2.0f, 2.0f)) * inv_mask);
  const float4 peak = max(max(a, b), max(c, d));
  return float4(max(max(peak.x, peak.y), max(peak.z, peak.w)) > g_push.shape.w ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f);
}

// True when the apply cannot use this pixel's result: every texel of the mask that the bilinear samples of the pixel and of
// its 8 neighbours reach is at most the limit, or at least every texel of the blocks that hold them is (contact_trace.h
// SkipTexelRange; MaskCannotLightBlocks is this function's twin). `position` is SV_Position: the pixel's centre, (pixel + 0.5).
bool MaskCannotLight(float2 position) {
  const uint slot = g_push.slots2.w;
  const float2 scale = g_push.sizes.xy;  // mask texels per pixel over kSkipBlock: block images' texels per pixel
  // (1) The block the pixel's own centre falls in is inside the range below: a 1 there, and the pixel is marched.
  if (g_textures[slot].Load(int3(int2(position * scale), 0)).x > 0.5f) {
    return false;
  }
  // (2) The range of texels, first to end (SkipTexelRange: (pixel - 0.5) s - 0.5 - margin to (pixel + 1.5) s - 0.5 + margin, the floor
  // of the low end to the floor of the high end + 1, s the mask's texels a pixel), in blocks: over kSkipBlock, the floor of the low
  // end to the floor of (high end + 1). Groups of 2 x 2 blocks from the first on; the sampler is clamp to edge, so a group at or
  // past the image's border repeats its border blocks (the mask's texels there are covered by the border blocks).
  const float2 lo = (position - 1.0f) * scale - (0.5f + kSkipTexelMargin) * (1.0f / kSkipBlock);
  const float2 first = floor(lo);
  const float2 end = floor(lo + (2.0f * scale + (2.0f * kSkipTexelMargin + 1.0f) * (1.0f / kSkipBlock)));
  const float2 inv_block = float2(g_push.extra.x, g_push.extra.z);
  [loop] for (float y = first.y; y <= end.y; y += 2.0f) {
    [loop] for (float x = first.x; x <= end.x; x += 2.0f) {
      // (x + 1, y + 1) blocks in is the corner between the group's four blocks: the gather returns x, x + 1 by y, y + 1.
      const float4 group = g_textures[slot].GatherRed(g_samplers[kSamplerLinearClamp], (float2(x, y) + 1.0f) * inv_block);
      if (any(group > 0.5f)) {
        return false;
      }
    }
  }
  return true;
}

// The statistics picture of one pixel (THE SKIP STATISTICS above): the march's early parts, no steps.
float4 SkipStat(float2 position) {
  const int2 pixel = int2(position);
  const float2 size = g_push.sizes.zw;
  const uint depth_slot = g_push.slots.z;
  const float tc = AoLoadLinear(depth_slot, pixel);
  if (tc >= kAoSky * 0.5f || tc >= g_push.extra.y) {
    return float4(kStatFar, 0.0f, 0.0f, 0.0f);
  }
  if ((g_push.slots2.z & kSkipFlag) != 0u && MaskCannotLight(position) && !VehicleInReach(depth_slot, pixel, int2(size), tc)) {
    return float4(kStatSkipped, 0.0f, 0.0f, 0.0f);
  }
  const float3 pc = AoViewPos(float2(pixel) + 0.5f, size, tc);
  float3 normal = AoNormalAt(g_push.slots.y >> 16, depth_slot, pixel, int2(size), tc, pc);
  if (dot(normal, pc) > 0.0f) {
    normal = -normal;
  }
  const float3 to_sun = g_push.light.xyz;
  const float facing = dot(normal, to_sun);
  const float stored = g_textures[g_push.slots.x].Load(int3(int2(position * (g_push.sizes.xy * kSkipBlock)), 0)).x;
  return float4(((facing <= 0.0f || dot(to_sun, to_sun) < 0.5f) ? kStatBack : kStatMarch) + stored, 0.0f, 0.0f, 0.0f);
}

float4 main(float4 position : SV_Position) : SV_Target {
  const int2 pixel = int2(position.xy);
  const float2 size = g_push.sizes.zw;
  const int2 isize = int2(size);
  const uint depth_slot = g_push.slots.z;
  const uint flags = g_push.slots2.z;
  if ((flags & kSideFlag) != 0u) {
    return (flags & kStatFlag) != 0u ? SkipStat(position.xy) : SkipBlocks(position.xy);
  }
  const float tc = AoLoadLinear(depth_slot, pixel);
  if (tc >= kAoSky * 0.5f || tc >= g_push.extra.y) {
    return float4(1.0f, 0.0f, 0.0f, 0.0f);
  }
  // Under the game's own shadow the apply cannot use the result (THE SKIP above): before the normal's four reads.
  // A vehicle's pixel, and a pixel beside one, is marched whatever the mask says (THE GAME'S MASK IS NOT A CAR'S above).
  if ((flags & kSkipFlag) != 0u && MaskCannotLight(position.xy) && !VehicleInReach(depth_slot, pixel, isize, tc)) {
    return float4(1.0f, 0.0f, 0.0f, 0.0f);
  }
  const float3 pc = AoViewPos(float2(pixel) + 0.5f, size, tc);
  float3 normal = AoNormalAt(g_push.slots.y >> 16, depth_slot, pixel, isize, tc, pc);
  if (dot(normal, pc) > 0.0f) {
    normal = -normal;  // facing the camera
  }
  const float3 to_sun = g_push.light.xyz;
  // A surface that faces away from the sun gets none of its light: there is nothing to shadow, and
  // the apply pass's sun share is 0 there as well.
  const float facing = dot(normal, to_sun);
  if (facing <= 0.0f || dot(to_sun, to_sun) < 0.5f) {
    return float4(1.0f, 0.0f, 0.0f, 0.0f);
  }

  float length_units = g_push.lens.z;
  float thickness = max(g_push.lens.w, 1e-3f);
  const float fade_start = min(saturate(g_push.shape.x), kFadeStartMax);
  uint steps = uint(max(g_push.shape.y, 1.0f));
  if (AoIsVehicle(tc)) {
    // A car's own pixel (a trunk lid under a wing, a door, a roof): a short, thin ray, so that a wing's plate and its posts, which
    // are thinner than the thickness, do not hide the ray from the camera by overlapping it on the screen (THE CAR'S OWN PIXELS above).
    length_units = min(length_units, kVehicleLength);
    thickness = min(thickness, kVehicleThickness);
    steps = min(steps, uint(kVehicleSteps));
  }
  // The world size of one pixel of the depth image at distance 1 (two tan of the half field of view
  // over the height): times the distance, a footprint.
  const float footprint_unit = 2.0f * g_push.lens.y / size.y;
  // Interleaved gradient noise plus the frame's R2 shift (ao_main_ps): the start of the ray inside
  // its first step.
  const float frame = float(g_push.slots2.y & 1023u);
  const float noise = frac(AoNoise(float2(pixel)) + frame * 0.75487766624669f);

  // The ray traced term (THE RAY TRACED TERM above): 1 where the sun's ray from this pixel meets nothing of the car, 0 where it meets a part.
  const bool traced = (flags & kRtContactFlag) != 0u;
  float traced_shadow = 0.0f;
  if (traced) {
    traced_shadow = 1.0f - g_textures[g_push.slots.x].SampleLevel(g_samplers[kSamplerLinearClamp], (float2(pixel) + 0.5f) / size, 0).y;
    if ((flags & kRtContactViewFlag) != 0u) {
      // The debug view: the traced term alone.
      return float4(1.0f - traced_shadow * smoothstep(kSunFacingLow, kSunFacingHigh, facing), 0.0f, 0.0f, 0.0f);
    }
  }

  // The car's own shadow map (THE CAR'S SHADOW MAP above): the shadow of the car's own parts, exact, and which pixels it is the truth for. A
  // surface this nearly edge-on to the sun gets no contact shadow whatever made it (the weight at the end is 0): the map is not read there.
  const bool car_map = (flags & kCarMapFlag) != 0u;
  float car_shadow = 0.0f;
  if (car_map) {
    bool covered = false;
    if (facing > kSunFacingLow) {
      car_shadow = CarMapShadow(pc, normal, footprint_unit * tc, noise, covered);
    }
    if ((flags & kCarMapViewFlag) != 0u) {
      // The debug view: the map's term alone.
      return float4(1.0f - car_shadow * smoothstep(kSunFacingLow, kSunFacingHigh, facing), 0.0f, 0.0f, 0.0f);
    }
    if (covered && AoIsVehicle(tc) && (flags & kCarMapKeepMarchFlag) == 0u) {
      steps = 0u;  // the map holds the car's own parts exactly: the short vehicle march has nothing to add on this pixel
    }
  }

  const float3 start = pc + normal * (kLiftFloor + kLiftPixels * footprint_unit * tc);
  // The adaptive step count: as many steps as the ray's length on the screen needs (contact_trace.h AdaptiveSteps).
  const float end_gap = g_push.shape.z;
  if (end_gap > 0.0f) {
    const float3 ray_end = start + to_sun * length_units;
    if (start.z > kNearZ && ray_end.z > kNearZ) {
      const float2 a = float2(start.x / (start.z * g_push.lens.x), start.y / (start.z * g_push.lens.y));
      const float2 b = float2(ray_end.x / (ray_end.z * g_push.lens.x), ray_end.y / (ray_end.z * g_push.lens.y));
      const float want = ceil(2.0f * length((b - a) * (0.5f * size)) / end_gap);
      if (want < float(steps)) {
        steps = max(uint(want), min(uint(kAdaptMinSteps), steps));
      }
    }
  }
  const float inv_steps = 1.0f / float(steps);
  // A vehicle pixel that marches (the map did not take its steps) stores the marked value: THE VEHICLE MARCH'S STIPPLE above. Not one the traced sun ray holds in a
  // shadow: that term is exact and its edge stays crisp (THE MARK AND THE RAY TRACED TERM above). traced_shadow is 0 without the term, so the test is false then.
  const bool marked = AoIsVehicle(tc) && steps > 0u && traced_shadow < kRtUnmarkedShadow;
  // The ground under a vehicle is in its shadow before the ray looks (0 for everything else).
  float shadow = Under(depth_slot, tc, pc, normal, to_sun, size, isize, footprint_unit, noise);
  for (uint i = 0u; i < steps; ++i) {
    if (shadow >= 0.99f) {
      break;
    }
    // The distance along the ray grows as the square of the step number: fine near the contact.
    const float t = (float(i) + noise) * inv_steps;
    const float along = t * t;
    const float3 p = start + to_sun * (along * length_units);
    if (p.z <= kNearZ) {
      break;
    }
    const float2 uv = float2(0.5f + 0.5f * p.x / (p.z * g_push.lens.x),
                             0.5f - 0.5f * p.y / (p.z * g_push.lens.y));
    if (any(uv < 0.0f) || any(uv >= 1.0f)) {
      break;
    }
    const float td = AoLoadLinear(depth_slot, min(int2(uv * size), isize - 1));
    // > 0: the surface at this pixel is in front of the step (it covers it from the camera).
    const float dz = p.z - td;
    const float footprint = footprint_unit * p.z;
    const float step_length = 2.0f * t * length_units * inv_steps;
    const float eps = kEpsFloor + kEpsPixels * footprint;
    const float ramp = kRampFloor + kRampPixels * footprint +
                       kRampStepShare * step_length * abs(to_sun.z);
    float hit = saturate((dz - eps) / ramp);
    hit *= 1.0f - saturate((dz - thickness) / thickness);
    hit *= 1.0f - smoothstep(fade_start, 1.0f, along);
    shadow = max(shadow, hit);
    if (shadow >= 0.99f) {
      break;
    }
  }
  if (car_map) {
    shadow = max(shadow, car_shadow);  // the darker of the march and the car's map
  }
  if (traced) {
    shadow = max(shadow, traced_shadow);  // ... and of the car's triangles
  }
  shadow *= smoothstep(kSunFacingLow, kSunFacingHigh, facing);
  const float visibility = 1.0f - shadow;
  return float4(marked ? visibility - kVehicleMarkOffset : visibility, 0.0f, 0.0f, 0.0f);
}
