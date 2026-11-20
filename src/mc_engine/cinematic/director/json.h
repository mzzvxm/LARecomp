#pragma once
//
// A small JSON value for the Director's wire format (DIRECTOR-DESIGN.md section 2.3; documentation/DIRECTOR-PROTOCOL.md). Pure: no game, no
// log, no third-party library, so the protocol's tests build it on their own.
//
// The wire is JSON lines: one object per line, UTF-8, at most kMaxLineBytes. This is the whole JSON the Director needs: null, bool, number
// (a double: every integer up to 2^53 is exact), string, array, object. An object keeps its members in the order they were written (the
// replies of `help` and the events read the same way every time). Parse never throws; Dump never fails (NaN and infinity are written as null,
// invalid UTF-8 bytes as U+FFFD).
//

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mc::cinematic::director {

inline constexpr size_t kMaxLineBytes = 1u << 20;  // one wire line, 1 MiB (the newline not counted)
inline constexpr int kMaxJsonDepth = 64;

class Json {
  public:
    enum class Type : uint8_t { kNull, kBool, kNumber, kString, kArray, kObject };
    using Member = std::pair<std::string, Json>;

    Json() = default;
    Json(std::nullptr_t) {}
    Json(bool v) : type_(Type::kBool), bool_(v) {}
    Json(int v) : type_(Type::kNumber), num_(v) {}
    Json(unsigned v) : type_(Type::kNumber), num_(v) {}
    Json(long v) : type_(Type::kNumber), num_(static_cast<double>(v)) {}
    Json(unsigned long v) : type_(Type::kNumber), num_(static_cast<double>(v)) {}
    Json(long long v) : type_(Type::kNumber), num_(static_cast<double>(v)) {}
    Json(unsigned long long v) : type_(Type::kNumber), num_(static_cast<double>(v)) {}
    Json(double v) : type_(Type::kNumber), num_(v) {}
    Json(const char* v) : type_(Type::kString), str_(v ? v : "") {}
    Json(std::string v) : type_(Type::kString), str_(std::move(v)) {}
    Json(std::string_view v) : type_(Type::kString), str_(v) {}

    static Json Array() {
        Json j;
        j.type_ = Type::kArray;
        return j;
    }
    static Json Object() {
        Json j;
        j.type_ = Type::kObject;
        return j;
    }
    // [x, y, z]
    static Json Vec3(double x, double y, double z) {
        Json j = Array();
        j.Push(x);
        j.Push(y);
        j.Push(z);
        return j;
    }

    Type type() const { return type_; }
    bool is_null() const { return type_ == Type::kNull; }
    bool is_bool() const { return type_ == Type::kBool; }
    bool is_number() const { return type_ == Type::kNumber; }
    bool is_string() const { return type_ == Type::kString; }
    bool is_array() const { return type_ == Type::kArray; }
    bool is_object() const { return type_ == Type::kObject; }
    // A number with no fractional part that a 64-bit integer holds.
    bool is_integer() const;

    // The value as the given type, or `fallback` when it is of another type.
    bool AsBool(bool fallback = false) const { return type_ == Type::kBool ? bool_ : fallback; }
    double AsNumber(double fallback = 0.0) const { return type_ == Type::kNumber ? num_ : fallback; }
    int64_t AsInt(int64_t fallback = 0) const;
    const std::string& AsString() const;  // "" when not a string

    // Arrays.
    size_t size() const { return type_ == Type::kArray ? items_.size() : type_ == Type::kObject ? members_.size() : 0; }
    const Json& at(size_t i) const;  // a null value when out of range
    Json& Push(Json value);          // makes this an array when it is null
    const std::vector<Json>& items() const { return items_; }

    // Objects.
    const Json* Find(std::string_view key) const;  // null when absent or not an object
    bool Has(std::string_view key) const { return Find(key) != nullptr; }
    const Json& Get(std::string_view key) const;  // a null value when absent
    Json& Set(std::string_view key, Json value);  // replaces a member of that name; makes this an object when it is null
    bool Erase(std::string_view key);
    const std::vector<Member>& members() const { return members_; }

    // Typed reads of an object's member: `fallback` when absent or of another type.
    double NumberOr(std::string_view key, double fallback) const;
    int64_t IntOr(std::string_view key, int64_t fallback) const;
    bool BoolOr(std::string_view key, bool fallback) const;
    std::string StringOr(std::string_view key, std::string_view fallback) const;

    // A number triple as [x, y, z]: false when this is not an array of three finite numbers.
    bool ToVec3(double out[3]) const;

    // One line, no newline, no spaces after separators.
    std::string Dump() const;
    void DumpTo(std::string* out) const;

    // Parses one whole value (anything after it but blanks is an error). On failure `error` says what and where ("line 1, column 14: ...").
    static bool Parse(std::string_view text, Json* out, std::string* error);

    bool operator==(const Json& other) const;

  private:
    Type type_ = Type::kNull;
    bool bool_ = false;
    double num_ = 0.0;
    std::string str_;
    std::vector<Json> items_;
    std::vector<Member> members_;
};

// A string as a JSON literal ("..." with the escapes), appended to `out`.
void AppendJsonString(std::string_view text, std::string* out);

// A number the way Dump writes it: an integer without a point when it is one (and small), else the shortest text that reads back as the same double.
std::string FormatNumber(double v);

}  // namespace mc::cinematic::director
