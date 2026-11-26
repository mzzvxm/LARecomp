float3 Dn2ShadowPosition(int2 p) {
  return Dn2Position(float2(p), RtDnDepthAt(p), Dn2Lens());
}

RtDnPixel RtDnLoad(int2 p) {
  RtDnPixel c = (RtDnPixel)0;
  if (RtDnW(24) != 0u) {
    const int2 lo = int2(RtDnLo(RtDnW(23)), RtDnHi(RtDnW(23)));
    const int2 hi = int2(RtDnLo(RtDnW(24)), RtDnHi(RtDnW(24)));
    if (any(p < lo) || any(p >= hi)) return c;
  }
  const float z = RtDnDepthAt(p);
  if (!(z > 0.0f && z < RtDnF(28))) return c;
  const int2 last = RtDnSize() - 1;
  c.pos = Dn2ShadowPosition(p);
  const float3 l = c.pos - Dn2ShadowPosition(max(p - int2(1, 0), 0));
  const float3 r = Dn2ShadowPosition(min(p + int2(1, 0), last)) - c.pos;
  const float3 u = Dn2ShadowPosition(max(p - int2(0, 1), 0)) - c.pos;
  const float3 d = c.pos - Dn2ShadowPosition(min(p + int2(0, 1), last));
  const float3 n = cross(dot(l, l) > 0.0f && (abs(l.z) < abs(r.z) || p.x == last.x) ? l : r,
                         dot(u, u) > 0.0f && (abs(u.z) < abs(d.z) || p.y == last.y) ? u : d);
  c.ok = true;
  c.z = z;
  c.rough = c.share = 1.0f;
  c.n = dot(n, n) > 1.0e-14f ? normalize(n) : float3(0.0f, 0.0f, -1.0f);
  if (dot(c.n, c.pos) > 0.0f) c.n = -c.n;
  c.colour = RtDnSignal(p).rgb;
  return c;
}
