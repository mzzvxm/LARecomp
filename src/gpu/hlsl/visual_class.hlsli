// Shared with capture_visual_test.cpp. material is the byte from G-buffer RT3.g.
uint VisualClass(uint material, bool sky, bool ui) {
  if (ui) return 7u;
  const uint cls = material & 31u;
  const bool vehicle = (material & 128u) != 0u;
  if (cls == GB_CLASS_NONE && sky) return 0u;
  if (cls == GB_CLASS_ROAD || cls == GB_CLASS_WET_ROAD) return 1u;
  if (cls == GB_CLASS_FOLIAGE) return 6u;
  if (cls == GB_CLASS_INTERIOR) return vehicle ? 4u : 5u;
  if (vehicle && cls == GB_CLASS_GLASS) return 3u;
  if (cls == GB_CLASS_PAINT || (vehicle && (cls == GB_CLASS_CHROME || cls == GB_CLASS_METAL))) return 2u;
  return 255u;
}
