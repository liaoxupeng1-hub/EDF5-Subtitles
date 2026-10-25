#include "SubtitleTable.h"

#include <fstream>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>

namespace edf5sub {
namespace {

constexpr std::size_t kMaxFileBytes = 64U * 1024U * 1024U;
constexpr std::size_t kMaxNodes = 1000000;
constexpr std::size_t kMaxEntries = 100000;
constexpr std::size_t kMaxStringUnits = 1024U * 1024U;
constexpr std::size_t kMaxDecodedBytes = 64U * 1024U * 1024U;
constexpr std::string_view kNoSubtitle = "***no_subtitle***";

class ParseError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// This parser follows only root -> table -> record -> strings. There is no
// recursive traversal, evaluation, or execution of input data.
class Reader {
public:
    explicit Reader(const std::vector<std::uint8_t>& bytes) : bytes_(bytes) {
        if (bytes_.size() < 16 || bytes_.size() > kMaxFileBytes)
            Fail("DSGO file size is outside the supported limits");
        if (bytes_[0] != 'D' || bytes_[1] != 'S' || bytes_[2] != 'G' ||
            bytes_[3] != 'O')
            Fail("Expected little-endian DSGO; other formats are unsupported");
        node_start_ = U32(4);
        node_count_ = U32(8);
        const auto root_offset = U32(12);
        if (node_start_ < 16 || (node_start_ & 7U) != 0 ||
            node_count_ == 0 || node_count_ > kMaxNodes)
            Fail("Invalid DSGO node array");
        Range(node_start_, node_count_ * 16U);
        if (root_offset < node_start_ ||
            (root_offset - node_start_) % 16U != 0 ||
            (root_offset - node_start_) / 16U >= node_count_)
            Fail("Invalid DSGO root node offset");
        root_index_ = (root_offset - node_start_) / 16U;
    }

    struct Child {
        std::string name;
        std::size_t node;
    };

    std::size_t Root() const noexcept { return root_index_; }

    std::vector<Child> Pointer(std::size_t index, std::size_t max_children) {
        const auto data = NodeData(index, 3);
        Range(data, 16);
        const auto names_relative = U32(data);
        const std::size_t count = U32(data + 4);
        const auto values_relative = U32(data + 8);
        const std::size_t names_count = U32(data + 12);
        if (count > max_children || count != names_count)
            Fail("Invalid DSGO pointer child/name count");
        const auto values = Relative(data, values_relative, count * 4U);
        const auto names = Relative(data, names_relative, count * 8U);
        std::vector<bool> named(count, false);
        std::set<std::string, std::less<>> unique_names;
        std::vector<Child> children;
        children.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            const auto record = names + i * 8U;
            const std::size_t position = U32(record + 4);
            if (position >= count || named[position])
                Fail("Invalid or repeated DSGO child position");
            named[position] = true;
            auto name = Utf16(Relative(record, U32(record), 2));
            if (name.empty() || !unique_names.insert(name).second)
                Fail("Empty or duplicate DSGO child name");
            const std::size_t child_index = U32(values + position * 4U);
            if (child_index >= node_count_)
                Fail("DSGO child node index is out of range");
            children.push_back({std::move(name), child_index});
        }
        return children;
    }

    std::string String(std::size_t index) {
        return Utf16(NodeData(index, 1));
    }

private:
    [[noreturn]] static void Fail(const char* message) {
        throw ParseError(message);
    }

    void Range(std::size_t offset, std::size_t length) const {
        if (offset > bytes_.size() || length > bytes_.size() - offset)
            Fail("DSGO offset or length is outside the file");
    }

    std::size_t Relative(std::size_t origin, std::uint64_t delta,
                         std::size_t length) const {
        if (origin > bytes_.size() || delta > bytes_.size() - origin)
            Fail("DSGO relative offset is outside the file");
        const auto result = origin + static_cast<std::size_t>(delta);
        Range(result, length);
        return result;
    }

    std::uint32_t U32(std::size_t offset) const {
        Range(offset, 4);
        std::uint32_t result = 0;
        for (unsigned i = 0; i < 4; ++i)
            result |= static_cast<std::uint32_t>(bytes_[offset + i]) << (8U * i);
        return result;
    }

    std::uint64_t U64(std::size_t offset) const {
        Range(offset, 8);
        std::uint64_t result = 0;
        for (unsigned i = 0; i < 8; ++i)
            result |= static_cast<std::uint64_t>(bytes_[offset + i]) << (8U * i);
        return result;
    }

    std::size_t NodeData(std::size_t index, std::uint64_t type) const {
        if (index >= node_count_) Fail("DSGO node index is out of range");
        const auto node = node_start_ + index * 16U;
        if (U64(node + 8) != type) Fail("Unexpected DSGO node type");
        return Relative(node, U64(node), type == 1 ? 2U : 16U);
    }

    std::string Utf16(std::size_t offset) {
        if ((offset & 1U) != 0) Fail("Unaligned DSGO UTF-16 string");
        std::string result;
        std::size_t units = 0;
        const auto next_unit = [&]() -> std::uint32_t {
            if (++units > kMaxStringUnits)
                Fail("DSGO string exceeds the supported length");
            Range(offset, 2);
            const auto value = static_cast<std::uint32_t>(bytes_[offset]) |
                               (static_cast<std::uint32_t>(bytes_[offset + 1]) << 8U);
            offset += 2;
            return value;
        };
        for (;;) {
            auto point = next_unit();
            if (point == 0) break;
            if (point >= 0xD800 && point <= 0xDBFF) {
                const auto low = next_unit();
                if (low < 0xDC00 || low > 0xDFFF)
                    Fail("Unpaired UTF-16 high surrogate in DSGO string");
                point = 0x10000U + ((point - 0xD800U) << 10U) + (low - 0xDC00U);
            } else if (point >= 0xDC00 && point <= 0xDFFF) {
                Fail("Unpaired UTF-16 low surrogate in DSGO string");
            }
            const std::size_t needed = point <= 0x7FU ? 1U : point <= 0x7FFU ? 2U : point <= 0xFFFFU ? 3U : 4U;
            if (needed > kMaxDecodedBytes - decoded_bytes_)
                Fail("DSGO decoded strings exceed the supported total size");
            decoded_bytes_ += needed;
            if (needed == 1) {
                result.push_back(static_cast<char>(point));
            } else {
                result.push_back(static_cast<char>((needed == 2 ? 0xC0U : needed == 3 ? 0xE0U : 0xF0U) |
                                                  (point >> (6U * (needed - 1U)))));
                for (std::size_t part = needed - 1; part != 0; --part)
                    result.push_back(static_cast<char>(0x80U | ((point >> (6U * (part - 1U))) & 0x3FU)));
            }
        }
        return result;
    }

    const std::vector<std::uint8_t>& bytes_;
    std::size_t node_start_ = 0;
    std::size_t node_count_ = 0;
    std::size_t root_index_ = 0;
    std::size_t decoded_bytes_ = 0;
};

void SetError(std::string* error, const char* message) {
    if (error) *error = message;
}

}  // namespace

bool SubtitleTable::LoadFile(const std::filesystem::path& path, std::string* error) {
    Clear();
    if (error) error->clear();
    try {
        std::ifstream stream(path, std::ios::binary | std::ios::ate);
        if (!stream) {
            SetError(error, "Cannot open subtitle table for reading");
            return false;
        }
        const auto end = stream.tellg();
        if (end < std::streampos(16) || end > std::streampos(kMaxFileBytes)) {
            SetError(error, "Subtitle table file size is outside the supported limits");
            return false;
        }
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(end));
        stream.seekg(0, std::ios::beg);
        if (!stream.read(reinterpret_cast<char*>(bytes.data()),
                         static_cast<std::streamsize>(bytes.size()))) {
            SetError(error, "Cannot read the complete subtitle table");
            return false;
        }
        return LoadBytes(bytes, error);
    } catch (const std::exception& exception) {
        SetError(error, exception.what());
        return false;
    }
}

bool SubtitleTable::LoadBytes(const std::vector<std::uint8_t>& bytes,
                             std::string* error) {
    Clear();
    if (error) error->clear();
    try {
        Reader reader(bytes);
        auto root = reader.Pointer(reader.Root(), 32);
        const Reader::Child* table = nullptr;
        for (const auto& child : root) {
            if (child.name == "table") table = &child;
        }
        if (!table) throw ParseError("DSGO root has no table variable");
        if (table->node == reader.Root()) throw ParseError("Cyclic DSGO root/table pointer");
        const auto entries = reader.Pointer(table->node, kMaxEntries);
        if (entries.empty()) throw ParseError("DSGO subtitle table is empty");
        std::map<std::string, SubtitleRecord, std::less<>> parsed;
        std::set<std::string, std::less<>> silent;
        for (const auto& entry : entries) {
            if (entry.node == reader.Root() || entry.node == table->node)
                throw ParseError("Cyclic DSGO subtitle entry pointer");
            const auto fields = reader.Pointer(entry.node, 2);
            if (fields.size() != 2) throw ParseError("Subtitle record must contain name and caption");
            SubtitleRecord record;
            record.id = entry.name;
            bool got_name = false, got_caption = false;
            for (const auto& field : fields) {
                if (field.name == "name") {
                    record.name = reader.String(field.node);
                    got_name = true;
                } else if (field.name == "caption") {
                    record.caption = reader.String(field.node);
                    got_caption = true;
                } else {
                    throw ParseError("Unexpected field in DSGO subtitle record");
                }
            }
            if (!got_name || !got_caption)
                throw ParseError("Subtitle record is missing name or caption");
            if (record.caption.empty() || record.caption == kNoSubtitle) {
                silent.insert(record.id);
                continue;
            }
            const auto id = record.id;
            if (!parsed.emplace(id, std::move(record)).second)
                throw ParseError("Duplicate subtitle ID");
        }
        records_.swap(parsed);
        silent_ids_.swap(silent);
        return true;
    } catch (const std::exception& exception) {
        SetError(error, exception.what());
        return false;
    }
}

const SubtitleRecord* SubtitleTable::Find(std::string_view id) const noexcept {
    auto found = records_.find(id);
    if (found != records_.end()) return &found->second;
    if (silent_ids_.find(id) != silent_ids_.end()) return nullptr;
    if (id.size() > 2 && id[id.size() - 2] == '_' &&
        (id.back() == 'S' || id.back() == 'E' || id.back() == 'R')) {
        found = records_.find(id.substr(0, id.size() - 2));
        if (found != records_.end()) return &found->second;
    }
    return nullptr;
}

}  // namespace edf5sub
