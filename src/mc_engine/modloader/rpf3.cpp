#include "rpf3.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>

#include "aes256.h"

namespace mc::modloader {

namespace {

constexpr uint32_t kMagicRpf0 = 0x30465052u;  // 'RPF0'
constexpr uint32_t kMagicRpf3 = 0x33465052u;  // 'RPF3'
constexpr uint64_t kTocOffset = 2048;
constexpr uint64_t kAlign = 2048;
constexpr int kCryptRounds = 16;

uint64_t AlignUp(uint64_t value, uint64_t align) {
    return (value + align - 1) / align * align;
}

std::vector<std::string> SplitPath(std::string_view path) {
    std::vector<std::string> parts;
    std::string current;
    for (char c : path) {
        if (c == '/' || c == '\\') {
            if (!current.empty()) parts.push_back(current);
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    if (!current.empty()) parts.push_back(current);
    return parts;
}

}  // namespace

uint32_t RageHash(std::string_view text) {
    uint32_t h = 0;
    for (char raw : text) {
        uint8_t c = static_cast<uint8_t>(raw);
        if (c >= 'A' && c <= 'Z') c += 32;
        else if (c == '\\') c = '/';
        h += c;
        h += h << 10;
        h ^= h >> 6;
    }
    h += h << 3;
    h ^= h >> 11;
    h += h << 15;
    return h;
}

bool Rpf3Reader::Open(const std::filesystem::path& path) {
    entries_.clear();
    path_ = path;

    FILE* file = nullptr;
#if defined(_WIN32)
    if (_wfopen_s(&file, path.wstring().c_str(), L"rb") != 0) file = nullptr;
#else
    file = std::fopen(path.string().c_str(), "rb");
#endif
    if (!file) return false;

    uint32_t header[5] = {};
    if (std::fread(header, 4, 5, file) != 5) {
        std::fclose(file);
        return false;
    }
    if (header[0] < kMagicRpf0 || header[0] > kMagicRpf3) {
        std::fclose(file);
        return false;
    }

    const uint32_t toc_size = header[1];
    const uint32_t count = header[2];
    if (toc_size < static_cast<uint64_t>(count) * 16) {
        std::fclose(file);
        return false;
    }

    std::vector<uint8_t> toc(toc_size);
    if (std::fseek(file, static_cast<long>(kTocOffset), SEEK_SET) != 0 ||
        std::fread(toc.data(), 1, toc.size(), file) != toc.size()) {
        std::fclose(file);
        return false;
    }
    std::fclose(file);

    Aes256EcbDecrypt(toc.data(), toc.size(), kRpfKey, kCryptRounds);

    entries_.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t words[4];
        std::memcpy(words, toc.data() + static_cast<size_t>(i) * 16, 16);
        entries_[i] = Rpf3Entry{words[0], words[1], words[2], words[3]};
    }
    return true;
}

bool Rpf3Reader::Find(std::string_view path, Rpf3Entry& out) const {
    if (entries_.empty()) return false;

    const auto parts = SplitPath(path);
    if (parts.empty()) return false;

    Rpf3Entry current = entries_[0];
    for (size_t depth = 0; depth < parts.size(); ++depth) {
        if (!current.is_directory()) return false;

        const uint32_t hash = RageHash(parts[depth]);
        uint32_t low = current.first_child();
        uint32_t high = low + current.child_count();
        if (high > entries_.size()) return false;

        bool found = false;
        while (low < high) {
            const uint32_t mid = low + (high - low) / 2;
            const uint32_t mid_hash = entries_[mid].hash;
            if (mid_hash == hash) {
                current = entries_[mid];
                found = true;
                break;
            }
            if (mid_hash < hash) low = mid + 1;
            else high = mid;
        }
        if (!found) return false;
    }

    if (current.is_directory()) return false;
    out = current;
    return true;
}

bool Rpf3Reader::ListDirectory(std::string_view path, std::vector<Rpf3Entry>& out) const {
    out.clear();
    if (entries_.empty()) return false;

    Rpf3Entry current = entries_[0];
    for (const std::string& part : SplitPath(path)) {
        if (!current.is_directory()) return false;

        const uint32_t hash = RageHash(part);
        uint32_t low = current.first_child();
        uint32_t high = low + current.child_count();
        if (high > entries_.size()) return false;

        bool found = false;
        while (low < high) {
            const uint32_t mid = low + (high - low) / 2;
            const uint32_t mid_hash = entries_[mid].hash;
            if (mid_hash == hash) {
                current = entries_[mid];
                found = true;
                break;
            }
            if (mid_hash < hash) low = mid + 1;
            else high = mid;
        }
        if (!found) return false;
    }
    if (!current.is_directory()) return false;

    const uint32_t first = current.first_child();
    const uint32_t count = current.child_count();
    if (static_cast<uint64_t>(first) + count > entries_.size()) return false;
    out.assign(entries_.begin() + first, entries_.begin() + first + count);
    return true;
}

bool Rpf3Reader::ReadFile(const Rpf3Entry& entry, std::vector<uint8_t>& out) const {
    FILE* file = nullptr;
#if defined(_WIN32)
    if (_wfopen_s(&file, path_.wstring().c_str(), L"rb") != 0) file = nullptr;
#else
    file = std::fopen(path_.string().c_str(), "rb");
#endif
    if (!file) return false;

#if defined(_WIN32)
    const int seek_ok = _fseeki64(file, static_cast<int64_t>(entry.data_offset()), SEEK_SET);
#else
    const int seek_ok = fseeko(file, static_cast<off_t>(entry.data_offset()), SEEK_SET);
#endif
    if (seek_ok != 0) {
        std::fclose(file);
        return false;
    }

    out.resize(entry.size);
    const bool ok = out.empty() || std::fread(out.data(), 1, out.size(), file) == out.size();
    std::fclose(file);
    return ok;
}

void Rpf3Writer::Add(std::string path, std::vector<uint8_t> data, uint32_t flag,
                     uint32_t resource_type) {
    files_.push_back(PendingFile{std::move(path), std::move(data), flag, resource_type, false, 0});
}

void Rpf3Writer::AddHashed(std::string path, uint32_t name_hash, std::vector<uint8_t> data,
                           uint32_t flag, uint32_t resource_type) {
    files_.push_back(
        PendingFile{std::move(path), std::move(data), flag, resource_type, true, name_hash});
}

void Rpf3Writer::AddFromFile(std::string path, std::filesystem::path source, uint64_t size,
                             uint32_t flag, uint32_t resource_type) {
    PendingFile pending{std::move(path), {}, flag, resource_type, false, 0};
    pending.source = std::move(source);
    pending.source_size = size;
    files_.push_back(std::move(pending));
}

bool Rpf3Writer::Write(const std::filesystem::path& out_path) const {
    // Build the directory tree. Nodes are keyed by their full path so the same
    // directory mentioned by several files collapses into one node.
    struct Node {
        std::string name;
        bool is_dir = true;
        std::map<std::string, size_t> children;  // name -> node index
        const PendingFile* file = nullptr;

        uint32_t entry_index = 0;
        uint32_t first_child = 0;
        uint64_t data_offset = 0;
    };

    std::vector<Node> nodes;
    nodes.push_back(Node{});  // root

    // What a node is filed under. Normally its own name, but a file cloned out
    // of another archive keeps the hash that archive gave it -- its name was
    // never in any string table to begin with.
    auto hash_of = [](const Node& node) {
        return (node.file && node.file->hashed) ? node.file->name_hash : RageHash(node.name);
    };

    for (const auto& pending : files_) {
        const auto parts = SplitPath(pending.path);
        if (parts.empty()) return false;

        size_t node = 0;
        for (size_t i = 0; i < parts.size(); ++i) {
            const bool leaf = (i + 1 == parts.size());
            auto it = nodes[node].children.find(parts[i]);
            if (it == nodes[node].children.end()) {
                nodes.push_back(Node{});
                const size_t created = nodes.size() - 1;
                nodes[created].name = parts[i];
                nodes[created].is_dir = !leaf;
                nodes[created].file = leaf ? &pending : nullptr;
                nodes[node].children.emplace(parts[i], created);
                node = created;
            } else {
                node = it->second;
                if (leaf) return false;  // duplicate path
            }
        }
    }

    // Breadth-first index assignment: a directory's children must land in one
    // contiguous, hash-sorted run because FindEntry binary-searches them.
    std::vector<size_t> order;
    order.push_back(0);
    nodes[0].entry_index = 0;

    uint32_t next_index = 1;
    for (size_t cursor = 0; cursor < order.size(); ++cursor) {
        Node& parent = nodes[order[cursor]];
        if (!parent.is_dir) continue;

        std::vector<size_t> kids;
        kids.reserve(parent.children.size());
        for (const auto& [name, index] : parent.children) kids.push_back(index);
        std::sort(kids.begin(), kids.end(), [&](size_t a, size_t b) {
            return hash_of(nodes[a]) < hash_of(nodes[b]);
        });

        parent.first_child = next_index;
        for (size_t kid : kids) {
            nodes[kid].entry_index = next_index++;
            order.push_back(kid);
        }
    }

    const uint32_t entry_count = next_index;
    const uint32_t name_heap = 16;  // just the root's "/"
    const uint32_t toc_size =
        static_cast<uint32_t>(AlignUp(static_cast<uint64_t>(entry_count) * 16 + name_heap, 16));

    // Lay the payloads out after the TOC, each on a 2048-byte boundary since
    // the entry only stores offset/2048.
    uint64_t cursor = AlignUp(kTocOffset + toc_size, kAlign);
    for (size_t index : order) {
        Node& node = nodes[index];
        if (node.is_dir) continue;
        node.data_offset = cursor;
        cursor = AlignUp(cursor + node.file->payload_size(), kAlign);
    }
    const uint64_t total_size = cursor;

    std::vector<uint8_t> toc(toc_size, 0);
    for (size_t index : order) {
        const Node& node = nodes[index];
        uint32_t words[4] = {};

        if (node.is_dir) {
            const uint32_t child_count = static_cast<uint32_t>(node.children.size());
            words[0] = node.entry_index == 0 ? 0u : RageHash(node.name);
            words[1] = node.entry_index == 0 ? child_count : 0u;
            words[2] = 0x80000000u | node.first_child;
            words[3] = child_count;
        } else {
            if ((node.data_offset / kAlign) > 0x1FFFFFull) return false;  // 21-bit sector field
            words[0] = hash_of(node);
            words[1] = static_cast<uint32_t>(node.file->payload_size());
            // Eight bits of type, not eleven -- see Rpf3Entry::data_offset. The
            // payload is 2048-aligned, so the offset owns everything above bit
            // seven either way; masking wider than a byte leaks the type into
            // the offset and the streamer reads the wrong place.
            words[2] = static_cast<uint32_t>(node.data_offset) |
                       (node.file->resource_type & 0xFFu);
            words[3] = node.file->flag;
        }
        std::memcpy(toc.data() + static_cast<size_t>(node.entry_index) * 16, words, 16);
    }
    toc[static_cast<size_t>(entry_count) * 16] = '/';

    Aes256EcbEncrypt(toc.data(), toc.size(), kRpfKey, kCryptRounds);

    FILE* file = nullptr;
#if defined(_WIN32)
    if (_wfopen_s(&file, out_path.wstring().c_str(), L"wb") != 0) file = nullptr;
#else
    file = std::fopen(out_path.string().c_str(), "wb");
#endif
    if (!file) return false;

    const uint32_t header[5] = {kMagicRpf3, toc_size, entry_count, 0, 0xFFFFFFFFu};
    bool ok = std::fwrite(header, 4, 5, file) == 5;

    const std::vector<uint8_t> zeros(kAlign, 0);
    auto pad_to = [&](uint64_t target, uint64_t at) {
        while (ok && at < target) {
            const size_t chunk = static_cast<size_t>(std::min<uint64_t>(target - at, kAlign));
            ok = std::fwrite(zeros.data(), 1, chunk, file) == chunk;
            at += chunk;
        }
        return at;
    };

    uint64_t at = 20;
    at = pad_to(kTocOffset, at);
    if (ok) ok = std::fwrite(toc.data(), 1, toc.size(), file) == toc.size();
    at = kTocOffset + toc_size;

    std::vector<uint8_t> copy_buffer;
    for (size_t index : order) {
        const Node& node = nodes[index];
        if (node.is_dir || !ok) continue;
        at = pad_to(node.data_offset, at);
        if (!ok) break;

        if (node.file->on_disk()) {
            // Streamed a megabyte at a time: a music bank is several megabytes
            // and there can be hundreds of them in one archive.
            if (copy_buffer.empty()) copy_buffer.resize(1u << 20);
            FILE* input = nullptr;
#if defined(_WIN32)
            if (_wfopen_s(&input, node.file->source.wstring().c_str(), L"rb") != 0) input = nullptr;
#else
            input = std::fopen(node.file->source.string().c_str(), "rb");
#endif
            if (!input) {
                ok = false;
                break;
            }
            uint64_t left = node.file->source_size;
            while (left > 0) {
                const size_t want =
                    static_cast<size_t>(std::min<uint64_t>(left, copy_buffer.size()));
                if (std::fread(copy_buffer.data(), 1, want, input) != want) {
                    ok = false;
                    break;
                }
                if (std::fwrite(copy_buffer.data(), 1, want, file) != want) {
                    ok = false;
                    break;
                }
                left -= want;
            }
            std::fclose(input);
        } else {
            ok = std::fwrite(node.file->data.data(), 1, node.file->data.size(), file) ==
                 node.file->data.size();
        }
        at = node.data_offset + node.file->payload_size();
    }
    pad_to(total_size, at);

    std::fclose(file);
    return ok;
}

}  // namespace mc::modloader
