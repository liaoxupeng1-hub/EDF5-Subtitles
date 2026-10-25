#include "../Source/SubtitleTable.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using Bytes = std::vector<std::uint8_t>;
std::size_t checks = 0;

void Check(bool condition, const char* description) {
    ++checks;
    if (!condition) throw std::runtime_error(description);
}

void Put32(Bytes& bytes, std::size_t at, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes.at(at + i) = static_cast<std::uint8_t>(value >> (i * 8U));
}
void Put64(Bytes& bytes, std::size_t at, std::uint64_t value) {
    for (unsigned i = 0; i < 8; ++i) bytes.at(at + i) = static_cast<std::uint8_t>(value >> (i * 8U));
}
std::size_t AppendString(Bytes& bytes, const std::u16string& text) {
    if (bytes.size() & 1U) bytes.push_back(0);
    const auto offset = bytes.size();
    for (const auto unit : text) {
        bytes.push_back(static_cast<std::uint8_t>(unit));
        bytes.push_back(static_cast<std::uint8_t>(unit >> 8U));
    }
    bytes.insert(bytes.end(), {0, 0});
    return offset;
}

// Standalone fixtures express expected records rather than deriving expected
// output from the loader. Name records deliberately use reverse position order.
struct Example { std::u16string id, name, caption; };
Bytes Fixture(const std::vector<Example>& examples) {
    const auto nodes = 2U + examples.size() * 3U;
    Bytes bytes(16 + nodes * 16, 0);
    bytes[0] = 'D'; bytes[1] = 'S'; bytes[2] = 'G'; bytes[3] = 'O';
    Put32(bytes, 4, 16); Put32(bytes, 8, static_cast<std::uint32_t>(nodes)); Put32(bytes, 12, 16);
    const auto pointer = [&](std::size_t node, const std::vector<std::pair<std::u16string, std::uint32_t>>& children) {
        if (bytes.size() & 1U) bytes.push_back(0);
        const auto data = bytes.size();
        const auto count = children.size();
        bytes.resize(data + 16 + count * 12, 0);
        const auto node_at = 16 + node * 16;
        Put64(bytes, node_at, data - node_at); Put64(bytes, node_at + 8, 3);
        Put32(bytes, data, static_cast<std::uint32_t>(16 + count * 4));
        Put32(bytes, data + 4, static_cast<std::uint32_t>(count));
        Put32(bytes, data + 8, 16); Put32(bytes, data + 12, static_cast<std::uint32_t>(count));
        for (std::size_t i = 0; i < count; ++i) {
            Put32(bytes, data + 16 + i * 4, children[i].second);
            const auto position = count - 1 - i;
            const auto record = data + 16 + count * 4 + i * 8;
            const auto string = AppendString(bytes, children[position].first);
            Put32(bytes, record, static_cast<std::uint32_t>(string - record));
            Put32(bytes, record + 4, static_cast<std::uint32_t>(position));
        }
    };
    pointer(0, {{u"table", 1}});
    std::vector<std::pair<std::u16string, std::uint32_t>> entries;
    for (std::size_t i = 0; i < examples.size(); ++i)
        entries.emplace_back(examples[i].id, static_cast<std::uint32_t>(2 + i * 3));
    pointer(1, entries);
    for (std::size_t i = 0; i < examples.size(); ++i) {
        const auto node = 2 + i * 3;
        pointer(node, {{u"name", static_cast<std::uint32_t>(node + 1)},
                       {u"caption", static_cast<std::uint32_t>(node + 2)}});
        const std::array<std::u16string, 2> strings{{examples[i].name, examples[i].caption}};
        for (std::size_t j = 0; j < 2; ++j) {
            const auto at = 16 + (node + 1 + j) * 16;
            const auto string = AppendString(bytes, strings[j]);
            Put64(bytes, at, string - at); Put64(bytes, at + 8, 1);
        }
    }
    return bytes;
}

void SyntheticTests() {
    edf5sub::SubtitleTable table;
    std::string error;
    auto bytes = Fixture({{u"BASE", u"\u961f\u957f", u"\u4f60\u597d\U0001F680"},
                          {u"BASE_S", u"speaker", u"exact suffix variant"},
                          {u"BASE_E", u"speaker", u"***no_subtitle***"},
                          {u"EMPTY", u"speaker", u""}});
    Check(table.LoadBytes(bytes, &error), "Valid independent fixture must load");
    Check(error.empty(), "Successful load clears error");
    Check(table.Size() == 2, "Silent and empty captions are excluded");
    const auto* base = table.Find("BASE");
    Check(base && base->name == u8"队长" && base->caption == u8"你好🚀", "UTF-16 BMP and surrogate pair decode to UTF-8");
    Check(table.Find("BASE_R") == base, "Recognized _R falls back");
    Check(table.Find("BASE_S") && table.Find("BASE_S")->caption == "exact suffix variant", "Exact suffix entry takes priority");
    Check(!table.Find("BASE_E"), "Explicit silent exact ID suppresses fallback");
    Check(!table.Find("EMPTY") && !table.Find("EMPTY_S"), "Empty caption is not returned");
    Check(!table.Find("BASE_X") && !table.Find("BASE_s"), "Unknown and lowercase suffixes do not normalize");
    Check(!table.Find("") && !table.Find("_S") && !table.Find("B"), "Short and unknown keys fail cleanly");
    Check(table.Find("BASE_S_E") == table.Find("BASE_S"), "Only one terminal suffix is removed");
    Check(!table.LoadFile(std::filesystem::path("__missing_subtitle_table__.sgo"), &error), "Missing file fails");
    Check(table.Size() == 0 && !table.Find("BASE"), "Failed load clears stale table");

    const auto valid = Fixture({{u"ONE", u"name", u"caption"}});
    const auto reject = [&](Bytes bad, const char* what) {
        Check(!table.LoadBytes(bad, &error), what);
        Check(!error.empty() && table.Size() == 0 && !table.Find("ONE"), "Rejected data yields an error and no records");
    };
    reject({}, "Empty input fails");
    auto bad = valid; bad[0] = 'O'; reject(bad, "Other endian/format magic fails");
    bad = valid; Put32(bad, 8, 0xffffffffU); reject(bad, "Oversized node count fails");
    bad = valid; Put32(bad, 12, 17); reject(bad, "Unaligned root offset fails");
    bad = valid; Put64(bad, 16, ~std::uint64_t(0)); reject(bad, "Overflowing node offset fails");
    bad = valid; Put64(bad, 24, 1); reject(bad, "Incorrect root type fails");
    // Node array is 16 + 5 * 16 = 96 bytes; root pointer follows it.
    bad = valid; Put32(bad, 96 + 12, 2); reject(bad, "Mismatched pointer/name counts fail");
    bad = valid; Put32(bad, 96 + 16, 0xffffffffU); reject(bad, "Out-of-range child node fails");
    bad = valid; Put32(bad, 96 + 20 + 4, 1); reject(bad, "Out-of-range name position fails");
    bad = valid; Put32(bad, 96 + 16, 0); reject(bad, "Root/table cycle fails");
    reject(Fixture({{u"DUP", u"x", u"x"}, {u"DUP", u"y", u"y"}}), "Duplicate subtitle IDs fail");
    reject(Fixture({{u"ONE", u"name", std::u16string(1, char16_t(0xD800))}}), "Unpaired high surrogate fails");
    reject(Fixture({{u"ONE", u"name", std::u16string(1, char16_t(0xDC00))}}), "Unpaired low surrogate fails");
    bad = valid; bad.pop_back(); reject(bad, "Truncated UTF-16 terminator fails");
    bad = valid; bad.resize(64U * 1024U * 1024U + 1U); reject(std::move(bad), "Oversized file fails");

    auto owned_input = valid;
    Check(table.LoadBytes(owned_input), "Loader recovers after bad inputs");
    std::fill(owned_input.begin(), owned_input.end(), 0);
    Check(table.Find("ONE") && table.Find("ONE")->caption == "caption", "Loaded strings own their storage");
    table.Clear();
    Check(table.Size() == 0 && !table.Find("ONE"), "Clear removes all records");
}

void RealTableTests() {
    const auto* missionPath = _wgetenv(L"EDF6_TEST_MISSION_DIR");
    const auto* publishedPath = _wgetenv(L"EDF5_TEST_SUBTITLE_TABLE");
    if (!missionPath || !publishedPath) {
        std::cout << "SKIP optional real-table checks: set EDF6_TEST_MISSION_DIR and EDF5_TEST_SUBTITLE_TABLE\n";
        return;
    }
    const std::filesystem::path mission(missionPath), published(publishedPath);
    const std::array<std::filesystem::path, 7> paths{{
        mission / "EDF6_VOICETABLE.SC.SGO", mission / "EDF6_VOICETABLE.CN.SGO",
        mission / "EDF6_VOICETABLE.EN.SGO", mission / "EDF6_VOICETABLE.JA.SGO",
        mission / "EDF6_VOICETABLE.KR.SGO", mission / "EDF6_VOICETABLE.SGO", published}};
    for (std::size_t i = 0; i < paths.size(); ++i) {
        edf5sub::SubtitleTable table;
        std::string error;
        Check(table.LoadFile(paths[i], &error), error.empty() ? "Real subtitle table loads" : error.c_str());
        const auto expected = i == 5 ? 13872U : i == 3 ? 13868U : 13869U;
        Check(table.Size() == expected, "Real table retains expected displayable IDs");
        if (i == 3 || i == 5)
            Check(!table.Find("DLC_BEX10_002"), "Real explicit no_subtitle marker remains silent");
        const auto* news = table.Find("AE103_037");
        Check(news && !news->caption.empty(), "Real reporter caption exists");
        Check(table.Find("AE103_037_S") == news, "Previously missing _S reporter uses original record");
        Check(table.Find("AE103_037_E") == news && table.Find("AE103_037_R") == news, "_E and _R retain original records");
        Check(!table.Find("AM027W"), "Known absent AM027W remains absent");
        Check(!table.Find("FR_AE065B*"), "Wildcard request is not fabricated into a subtitle");
        if (i == 0 || i == 6)
            Check(table.Find("AE001_021_E") && table.Find("AE001_021_E")->caption == u8"有怪物啊！", "SC reference caption agrees with independently read original data");
        std::cout << "Real SGO " << i + 1 << ": " << table.Size() << " records OK\n";
    }
}
}  // namespace

int main() {
    try {
        SyntheticTests();
        RealTableTests();
        std::cout << "PASS " << checks << " subtitle table checks\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& exception) {
        std::cerr << "FAIL after " << checks << " checks: " << exception.what() << '\n';
        return EXIT_FAILURE;
    }
}
