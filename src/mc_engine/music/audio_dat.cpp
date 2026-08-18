#include "mc_engine/music/audio_dat.h"

#include <cstring>

namespace mc::music {
namespace {

uint32_t LoadBE32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | p[3];
}

void PushBE32(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>(value >> 24));
    out.push_back(static_cast<uint8_t>(value >> 16));
    out.push_back(static_cast<uint8_t>(value >> 8));
    out.push_back(static_cast<uint8_t>(value));
}

void StoreBE32(uint8_t* p, uint32_t value) {
    p[0] = static_cast<uint8_t>(value >> 24);
    p[1] = static_cast<uint8_t>(value >> 16);
    p[2] = static_cast<uint8_t>(value >> 8);
    p[3] = static_cast<uint8_t>(value);
}

}  // namespace

bool AudioDat::Parse(const std::vector<uint8_t>& blob, std::string& error) {
    const size_t n = blob.size();
    auto need = [&](size_t at, size_t bytes) { return at + bytes <= n; };

    if (!need(0, 8)) {
        error = "file is too short to hold a header";
        return false;
    }
    version_ = LoadBE32(blob.data());
    const uint32_t payload_size = LoadBE32(blob.data() + 4);
    if (!need(8, payload_size)) {
        error = "payload runs past the end of the file";
        return false;
    }
    payload_.assign(blob.begin() + 8, blob.begin() + 8 + payload_size);

    size_t p = 8 + payload_size;
    if (!need(p, 8)) {
        error = "no string section";
        return false;
    }
    const uint32_t section = LoadBE32(blob.data() + p);
    const uint32_t str_count = LoadBE32(blob.data() + p + 4);
    const size_t body = p + 8;
    if (section < 4 || !need(body, section - 4) || !need(body, 4ull * str_count)) {
        error = "string section runs past the end of the file";
        return false;
    }
    str_offsets_.resize(str_count);
    for (uint32_t i = 0; i < str_count; ++i) {
        str_offsets_[i] = LoadBE32(blob.data() + body + 4 * i);
    }
    const size_t str_base = body + 4ull * str_count;
    const size_t str_end = body + section - 4;
    if (str_base > str_end) {
        error = "string offsets overrun their own section";
        return false;
    }
    str_bytes_.assign(blob.begin() + str_base, blob.begin() + str_end);
    p = str_end;

    if (!need(p, 8)) {
        error = "no object index";
        return false;
    }
    const uint32_t obj_count = LoadBE32(blob.data() + p);
    name_blob_size_ = LoadBE32(blob.data() + p + 4);
    p += 8;

    objects_.clear();
    objects_.reserve(obj_count);
    for (uint32_t i = 0; i < obj_count; ++i) {
        if (!need(p, 1)) {
            error = "object index is truncated";
            return false;
        }
        const size_t len = blob[p++];
        if (!need(p, len + 8)) {
            error = "object index is truncated";
            return false;
        }
        Object object;
        object.name.assign(reinterpret_cast<const char*>(blob.data() + p), len);
        p += len;
        object.offset = LoadBE32(blob.data() + p);
        object.size = LoadBE32(blob.data() + p + 4);
        p += 8;
        objects_.push_back(std::move(object));
    }

    for (std::vector<uint32_t>* list : {&fixup_a_, &fixup_b_}) {
        if (!need(p, 4)) {
            error = "fixup list is truncated";
            return false;
        }
        const uint32_t count = LoadBE32(blob.data() + p);
        if (!need(p + 4, 4ull * count)) {
            error = "fixup list is truncated";
            return false;
        }
        list->resize(count);
        for (uint32_t i = 0; i < count; ++i) {
            (*list)[i] = LoadBE32(blob.data() + p + 4 + 4 * i);
        }
        p += 4 + 4ull * count;
    }

    if (p != n) {
        error = "parsed to " + std::to_string(p) + " of " + std::to_string(n) + " bytes";
        return false;
    }
    return true;
}

int AudioDat::IndexOf(std::string_view name) const {
    for (size_t i = 0; i < objects_.size(); ++i) {
        if (objects_[i].name == name) return static_cast<int>(i);
    }
    return -1;
}

std::vector<uint8_t> AudioDat::Record(std::string_view name) const {
    const int index = IndexOf(name);
    if (index < 0) return {};
    const Object& object = objects_[static_cast<size_t>(index)];
    if (static_cast<size_t>(object.offset) + object.size > payload_.size()) return {};
    return std::vector<uint8_t>(payload_.begin() + object.offset,
                                payload_.begin() + object.offset + object.size);
}

uint32_t AudioDat::AddString(const std::string& text) {
    for (size_t i = 0; i < str_offsets_.size(); ++i) {
        const uint32_t off = str_offsets_[i];
        if (off < str_bytes_.size() &&
            std::strcmp(reinterpret_cast<const char*>(str_bytes_.data() + off), text.c_str()) == 0) {
            return static_cast<uint32_t>(i);
        }
    }
    str_offsets_.push_back(static_cast<uint32_t>(str_bytes_.size()));
    str_bytes_.insert(str_bytes_.end(), text.begin(), text.end());
    str_bytes_.push_back(0);
    return static_cast<uint32_t>(str_offsets_.size() - 1);
}

uint32_t AudioDat::AddObject(const std::string& name, std::vector<uint8_t> record) {
    StoreBE32(record.data() + 1, name_blob_size_);
    const uint32_t offset = static_cast<uint32_t>(payload_.size());
    payload_.insert(payload_.end(), record.begin(), record.end());
    objects_.push_back(Object{name, offset, static_cast<uint32_t>(record.size())});
    name_blob_size_ += static_cast<uint32_t>(name.size()) + 1;
    return offset;
}

uint32_t AudioDat::ReplaceObject(std::string_view name, std::vector<uint8_t> record) {
    const int index = IndexOf(name);
    if (index < 0) return 0;
    Object& object = objects_[static_cast<size_t>(index)];
    StoreBE32(record.data() + 1, LoadBE32(payload_.data() + object.offset + 1));
    object.offset = static_cast<uint32_t>(payload_.size());
    object.size = static_cast<uint32_t>(record.size());
    payload_.insert(payload_.end(), record.begin(), record.end());
    return object.offset;
}

std::vector<uint8_t> AudioDat::Serialize() const {
    std::vector<uint8_t> out;
    out.reserve(payload_.size() + str_bytes_.size() + objects_.size() * 24 + 64);

    PushBE32(out, version_);
    PushBE32(out, static_cast<uint32_t>(payload_.size()));
    out.insert(out.end(), payload_.begin(), payload_.end());

    const uint32_t section =
        4 + 4 * static_cast<uint32_t>(str_offsets_.size()) + static_cast<uint32_t>(str_bytes_.size());
    PushBE32(out, section);
    PushBE32(out, static_cast<uint32_t>(str_offsets_.size()));
    for (uint32_t offset : str_offsets_) PushBE32(out, offset);
    out.insert(out.end(), str_bytes_.begin(), str_bytes_.end());

    PushBE32(out, static_cast<uint32_t>(objects_.size()));
    PushBE32(out, name_blob_size_);
    for (const Object& object : objects_) {
        out.push_back(static_cast<uint8_t>(object.name.size()));
        out.insert(out.end(), object.name.begin(), object.name.end());
        PushBE32(out, object.offset);
        PushBE32(out, object.size);
    }

    for (const std::vector<uint32_t>* list : {&fixup_a_, &fixup_b_}) {
        PushBE32(out, static_cast<uint32_t>(list->size()));
        for (uint32_t offset : *list) PushBE32(out, offset);
    }
    return out;
}

}  // namespace mc::music
