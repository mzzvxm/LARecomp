#ifndef MCLA_SSGI_FILTER_PLANE_HLSLI
#define MCLA_SSGI_FILTER_PLANE_HLSLI

// The view ray dotted with the surface normal is affine in pixel coordinates.
// Express the tap relative to the centre to avoid subtracting two world positions.
float SsgiFilterPlaneOffset(float tap_depth, float centre_depth, float centre_ray, float ray_step) {
  return (tap_depth - centre_depth) * centre_ray + tap_depth * ray_step;
}

#endif
