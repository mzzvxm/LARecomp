// Opaque coverage. Every fragment writes the same value, so a depth
// attachment and depth sorting would produce the same silhouette.
float main() : SV_Target {
  return 1.0;
}
