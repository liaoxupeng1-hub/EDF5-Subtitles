#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace edf5sub {

struct SubtitleRecord {
    std::string id;
    std::string name;
    std::string caption;
};

class SubtitleTable {
public:
    // A failed load clears previous records. Files are opened for reading only.
    bool LoadFile(const std::filesystem::path& path, std::string* error = nullptr);
    bool LoadBytes(const std::vector<std::uint8_t>& bytes,
                   std::string* error = nullptr);

    // Returned records remain valid until Clear or the next load.
    // Exact IDs take priority over the recognized terminal suffix fallback.
    const SubtitleRecord* Find(std::string_view id) const noexcept;
    std::size_t Size() const noexcept { return records_.size(); }
    template<class Visitor>
    void ForEachRecord(Visitor&& visitor) const {
        for (const auto& entry : records_) visitor(entry.second);
    }
    void Clear() noexcept { records_.clear(); silent_ids_.clear(); }

private:
    std::map<std::string, SubtitleRecord, std::less<>> records_;
    std::set<std::string, std::less<>> silent_ids_;
};

}  // namespace edf5sub
