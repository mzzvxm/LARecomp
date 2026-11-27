// The compiled layer is empty. The local census writes the runtime text table beside the player's shader pack.
// src/gpu/gbuffer_texture_rows.h describes the layers and the lookup.
inline constexpr uint32_t kCompiledRowsFormat = 1;
inline constexpr CompiledRow kCompiledHashRows[] = {
    {0, {}},  // unused sentinel: an empty table is a legal array
};
inline constexpr CompiledRow kCompiledPairRows[] = {
    {0, {}},  // unused sentinel: an empty table is a legal array
};
inline constexpr CompiledRow kCompiledNameRows[] = {
    {0, {}},  // unused sentinel: an empty table is a legal array
};
