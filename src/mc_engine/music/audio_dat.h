// The RAGE audio metadata container (audio/x360/config/{game,sounds}.dat).
//
// Loaded by sub_8214E210 in three passes; everything is big endian:
//
//     u32 version
//     u32 payloadSize ; payload[]
//     u32 strSectionSize ; u32 strCount
//     u32 strOffset[strCount] ; strBytes[]     (both inside strSectionSize - 4)
//     u32 objCount ; u32 nameBlobSize
//     objCount x { u8 nameLen ; char name[] ; u32 objOffset ; u32 objSize }
//     u32 fixupACount ; u32 offA[]
//     u32 fixupBCount ; u32 offB[]
//
// The object index is comb-sorted by name hash at load (sub_8214D730), so file
// order is free: a new object goes on the end and every existing record keeps
// its name-blob offset. Fixup A rewrites the dword at payload[off - 8] from a
// name hash into an offset relative to the payload; fixup B rewrites it into a
// string-table index, which is what a wave leaf uses to name its bank.
//
// This is the C++ side of tools/audiodat.py, whose `verify` round-trips
// byte-for-byte on all four shipped files.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace mc::music {

class AudioDat {
 public:
    struct Object {
        std::string name;
        uint32_t offset = 0;
        uint32_t size = 0;
    };

    bool Parse(const std::vector<uint8_t>& blob, std::string& error);

    int IndexOf(std::string_view name) const;
    // Bytes of a named record, empty when it is not there.
    std::vector<uint8_t> Record(std::string_view name) const;

    // Appends a string, returning its index; an existing one is reused.
    uint32_t AddString(const std::string& text);

    // Appends a record and its index entry. The record's name-blob offset at
    // +1 is filled in here; +5 stays 0 for the loader to rebase.
    uint32_t AddObject(const std::string& name, std::vector<uint8_t> record);

    // Writes a new, possibly longer body for an existing object. The old bytes
    // stay where they are as dead space, so nothing else in the payload shifts.
    uint32_t ReplaceObject(std::string_view name, std::vector<uint8_t> record);

    void AddFixupA(uint32_t payload_offset) { fixup_a_.push_back(payload_offset + 8); }
    void AddFixupB(uint32_t payload_offset) { fixup_b_.push_back(payload_offset + 8); }

    std::vector<uint8_t> Serialize() const;

    size_t object_count() const { return objects_.size(); }

 private:
    uint32_t version_ = 0;
    std::vector<uint8_t> payload_;
    std::vector<uint32_t> str_offsets_;
    std::vector<uint8_t> str_bytes_;
    uint32_t name_blob_size_ = 0;
    std::vector<Object> objects_;
    std::vector<uint32_t> fixup_a_;
    std::vector<uint32_t> fixup_b_;
};

}  // namespace mc::music
