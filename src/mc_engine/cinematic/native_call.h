#pragma once
//
// Calls one of the game's script natives from the host (trailer toolkit; the product form of the E4 call shim).
//
// A script native is a function the game's script machine runs from its CALL instruction. Its wrapper takes one argument, a
// context in guest memory that the machine builds on its stack, and finds everything else through it:
//
//     +0   guest pointer to the return slot (one 32-bit word)
//     +4   the argument count
//     +8   guest pointer to the argument array: 32-bit big-endian slots (integers, bools as 0/1, floats bit-copied, objects by address)
//     +12  the number of vector slots in use (the wrapper counts them)
//     +32  the vector slots, 16 bytes each: a native that RETURNS a vector (Racer_GetPosition) stores it in slot 0 (NativeResult::vec);
//          a native that TAKES a vector or a string gets a guest pointer in its argument slot (GuestPutVec, GuestPutString)
//
// The context, the return slot, the arguments and a small ring of vectors and strings live in one block taken once from the game's
// own allocator. Wrapper addresses are the second column of feature-parity/natives/natives.tsv.
//
// Only on the game thread (Patch_DeltaTimePre), at a point where the world is up; the caller decides what is safe for the native
// it calls. E4 called 21 read-only natives this way without a refusal; the natives that write (the ambient and police switches
// the scene runner uses) are exercised by the scene chain (CHECK until a run says so).
//

#include <cstdint>
#include <initializer_list>

namespace mc::cinematic {

struct NativeResult {
    bool called = false;        // false: no guest thread, no such function, no block to build the context in
    bool ret_written = false;   // the native wrote its return slot
    uint32_t ret = 0;           // the slot as the native left it
    double ms = 0.0;            // how long the call took
    uint32_t vec_count = 0;     // the vector slots the native used (0 to 4)
    float vec[4][4] = {};       // ... as it left them: x, y, z, w
};

// Up to 8 arguments, each one 32-bit slot. Natives that return nothing leave ret_written false.
NativeResult CallNative(uint32_t wrapper, std::initializer_list<uint32_t> args);

// Guest memory for what a native takes by pointer: a vector (x, y, z, w) or a C string. The block is a ring: a pointer stays good
// for the next 15 vectors and for about 500 bytes of strings, which is for the call being built, not for keeping. 0 when there is
// no block (no guest thread yet, or the allocator gave nothing).
uint32_t GuestPutVec(float x, float y, float z, float w);
uint32_t GuestPutString(const char* text);

}  // namespace mc::cinematic
