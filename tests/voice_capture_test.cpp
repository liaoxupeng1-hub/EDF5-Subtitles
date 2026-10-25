// Integration tests compile the real hook implementation into this executable.
// Only the original-game function pointers are replaced; no detours are installed.
#include "../Source/Runtime.cpp"

#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <set>
#include <stdexcept>
#include <thread>

namespace {
using namespace edf5sub;
int checks = 0;

void Check(bool value, const char* message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
}

struct Name {
    std::string backing;
    GameString game{};
    explicit Name(std::string value) : backing(std::move(value)) {
        game.length = backing.size();
        if (backing.size() <= 15) {
            game.capacity = 15;
            std::memcpy(game.data, backing.data(), backing.size());
        } else {
            game.capacity = backing.size();
            game.ptr = backing.c_str();
        }
    }
};

struct Native {
    std::array<unsigned char, 16> padding{};
    std::uint32_t id;
    explicit Native(std::uint32_t value) : id(value) {}
};
static_assert(offsetof(Native, id) == 0x10);

struct Rendezvous {
    std::mutex mutex;
    std::condition_variable condition;
    unsigned arrived = 0, generation = 0;
    void Wait() {
        std::unique_lock<std::mutex> lock(mutex);
        const auto before = generation;
        if (++arrived == 2) {
            arrived = 0;
            ++generation;
            condition.notify_all();
        } else {
            condition.wait(lock, [&] { return generation != before; });
        }
    }
};

struct Plan {
    std::vector<std::uint32_t> ids;
    bool succeeds = true;
    bool returnNullNative = false;
    unsigned originalCalls = 0;
    GameString* expectedName = nullptr;
    void* expectedOutput = nullptr;
    void* expectedSpatial = nullptr;
    int expectedA5 = 0;
    unsigned expectedA6 = 0;
    bool abiOkay = true;
    Rendezvous* rendezvous = nullptr;
    Plan* nested = nullptr;
    GameString* nestedName = nullptr;
};

void* __fastcall FakeCreate(void* bank, const void* request) {
    auto& plan = *static_cast<Plan*>(bank);
    return plan.returnNullNative ? nullptr : const_cast<void*>(request);
}

bool Execute(Plan& plan) {
    ++plan.originalCalls;
    if (plan.nested) {
        const bool child = HookPlay(plan.nested, plan.nestedName, plan.nested->expectedOutput);
        plan.abiOkay &= child == plan.nested->succeeds;
    }
    if (plan.rendezvous) plan.rendezvous->Wait();
    for (auto id : plan.ids) {
        auto native = std::make_unique<Native>(id);
        const void* returned = HookCreate(&plan, native.get());
        plan.abiOkay &= returned == (plan.returnNullNative ? nullptr : native.get());
        // The real hook must retain only the integer ID. Poison and destroy the
        // native object before the top-level hook performs Subtitle Submit.
        native->id = 0xdeadbeefu;
        native.reset();
    }
    return plan.succeeds;
}

bool __fastcall FakePlay(void* manager, GameString* name, void* output) {
    auto& plan = *static_cast<Plan*>(manager);
    plan.abiOkay &= name == plan.expectedName && output == plan.expectedOutput;
    return Execute(plan);
}

bool __fastcall FakeSurround(void* manager, GameString* name, void* spatial,
                            void* output, int a5, unsigned a6) {
    auto& plan = *static_cast<Plan*>(manager);
    plan.abiOkay &= name == plan.expectedName && output == plan.expectedOutput &&
        spatial == plan.expectedSpatial && a5 == plan.expectedA5 && a6 == plan.expectedA6;
    return Execute(plan);
}

std::vector<SubtitleInstance> Drain() {
    std::vector<SubtitleInstance> result;
    TakePending(result);
    return result;
}

void Reset() {
    Check(Drain().empty(), "previous test left pending subtitles");
    Check(currentCapture == nullptr, "TLS capture was not restored");
}

void BasicAndFailure() {
    Reset();
    Name voice("AE104_052_S");
    int output = 1;
    Plan plan;
    plan.ids = {0x00010003u};
    plan.expectedName = &voice.game;
    plan.expectedOutput = &output;
    Check(HookPlay(&plan, &voice.game, &output), "successful PlayVoice result lost");
    auto items = Drain();
    Check(plan.originalCalls == 1 && plan.abiOkay, "PlayVoice original ABI/forwarding changed");
    Check(items.size() == 1, "successful _S request did not submit");
    Check(items[0].voiceId == voice.backing, "submitted voice name changed");
    Check(items[0].playbackIds == plan.ids, "native ID was not copied before object destruction");
    const auto* row = table.Find(voice.backing);
    Check(row && items[0].caption == DisplayCaption(row->caption, false), "real table text/quote formatting not used");
    plan.succeeds = false;
    Check(!HookPlay(&plan, &voice.game, &output), "false PlayVoice result changed");
    Check(Drain().empty(), "failed PlayVoice committed captured ID");
    Check(currentCapture == nullptr, "failed PlayVoice leaked TLS scope");

    plan.succeeds = true;
    plan.ids.clear();
    Check(HookPlay(&plan, &voice.game, &output), "no-capture success result changed");
    Check(Drain().empty(), "successful request without native capture invented subtitle");
    plan.ids = {0xffffffffu};
    HookPlay(&plan, &voice.game, &output);
    Check(Drain().empty(), "invalid playback ID was submitted");
    plan.ids = {0x00010007u};
    plan.returnNullNative = true;
    HookPlay(&plan, &voice.game, &output);
    Check(Drain().empty(), "null native creation was submitted");

    Name missing("MISSING_TEST_ID");
    plan.returnNullNative = false;
    plan.expectedName = &missing.game;
    HookPlay(&plan, &missing.game, &output);
    Check(Drain().empty(), "unknown table ID produced a caption");
}

void NullOutputAndSurround() {
    Reset();
    Name voice("AE001_001_E");
    Plan plan;
    plan.ids = {0x00020003u, 0x00020005u, 0x00020003u};
    plan.expectedName = &voice.game;
    Check(HookPlay(&plan, &voice.game, nullptr), "null output changed successful result");
    auto items = Drain();
    Check(plan.abiOkay && items.size() == 1, "null output suppressed capture or changed ABI");
    Check(items[0].playbackIds == std::vector<std::uint32_t>({0x00020003u, 0x00020005u}), "capture IDs not deduplicated within request");

    int output = 0, spatial = 0;
    plan.expectedOutput = &output;
    plan.expectedSpatial = &spatial;
    plan.expectedA5 = -17;
    plan.expectedA6 = 0xfedcba98u;
    plan.ids = {0x00030003u};
    Check(HookSurround(&plan, &voice.game, &spatial, &output, -17, 0xfedcba98u), "SurroundVoice success changed");
    items = Drain();
    Check(plan.abiOkay && items.size() == 1, "SurroundVoice parameter ABI changed");
    const auto* row = table.Find(voice.backing);
    Check(row && items[0].caption == DisplayCaption(row->caption, true), "spatial caption format not selected");
    plan.succeeds = false;
    Check(!HookSurround(&plan, &voice.game, &spatial, &output, -17, 0xfedcba98u), "SurroundVoice failure changed");
    Check(Drain().empty(), "failed SurroundVoice committed caption");
}

void NestedAndRepeated() {
    Reset();
    Name outerName("AE001_001"), innerName("AE001_002");
    Plan outer, inner;
    outer.ids = {0x00040003u};
    inner.ids = {0x00050003u};
    outer.expectedName = &outerName.game;
    inner.expectedName = &innerName.game;
    outer.nested = &inner;
    outer.nestedName = &innerName.game;
    Check(HookPlay(&outer, &outerName.game, nullptr), "nested outer success changed");
    auto items = Drain();
    Check(outer.abiOkay && inner.abiOkay && items.size() == 2, "nested calls lost or changed ABI");
    Check(items[0].voiceId == innerName.backing && items[0].playbackIds == inner.ids, "inner name/ID leaked into outer capture");
    Check(items[1].voiceId == outerName.backing && items[1].playbackIds == outer.ids, "outer capture not restored after inner");
    Check(items[0].serial != items[1].serial && currentCapture == nullptr, "nested serial or TLS lifetime invalid");

    outer.nested = nullptr;
    HookPlay(&outer, &outerName.game, nullptr);
    outer.ids = {0x00040005u};
    HookPlay(&outer, &outerName.game, nullptr);
    items = Drain();
    Check(items.size() == 2 && items[0].voiceId == items[1].voiceId, "repeated same-name requests were collapsed");
    Check(items[0].serial != items[1].serial && items[0].playbackIds != items[1].playbackIds,
          "same-name independent playback identity was lost");
}

void ConcurrentCaptures() {
    Reset();
    Name firstName("AE001_001"), secondName("AE001_002");
    Plan first, second;
    first.expectedName = &firstName.game;
    second.expectedName = &secondName.game;
    Rendezvous barrier;
    first.rendezvous = second.rendezvous = &barrier;
    bool firstOkay = true, secondOkay = true;
    constexpr unsigned count = 64;
    auto run = [](Plan& plan, Name& name, unsigned prefix, bool& okay) {
        for (unsigned i = 0; i < count; ++i) {
            plan.ids = {prefix | (2 * i + 1)};
            okay &= HookPlay(&plan, &name.game, nullptr);
            okay &= currentCapture == nullptr;
        }
    };
    std::thread a(run, std::ref(first), std::ref(firstName), 0x00060000u, std::ref(firstOkay));
    std::thread b(run, std::ref(second), std::ref(secondName), 0x00070000u, std::ref(secondOkay));
    a.join();
    b.join();
    auto items = Drain();
    Check(firstOkay && secondOkay && first.abiOkay && second.abiOkay, "concurrent hook ABI/TLS failed");
    Check(items.size() == 2 * count, "concurrent submission lost items");
    std::set<std::uint64_t> serials;
    unsigned firstSeen = 0, secondSeen = 0;
    bool matching = true;
    for (const auto& item : items) {
        serials.insert(item.serial);
        matching &= item.playbackIds.size() == 1;
        if (item.playbackIds.size() != 1) continue;
        const auto slot = item.playbackIds[0] >> 16;
        if (item.voiceId == firstName.backing) { ++firstSeen; matching &= slot == 6; }
        else if (item.voiceId == secondName.backing) { ++secondSeen; matching &= slot == 7; }
        else matching = false;
    }
    Check(matching && firstSeen == count && secondSeen == count, "concurrent names/IDs crossed threads");
    Check(serials.size() == items.size(), "concurrent submissions reused serial identity");
    Check(currentCapture == nullptr, "concurrency altered main-thread TLS capture");
}
} // namespace

int main(int argc, char** argv) {
    try {
        originalPlay = FakePlay;
        originalSurround = FakeSurround;
        originalCreate = FakeCreate;
        std::string error;
        const auto file = argc > 1 ? argv[1] : "build/preview/EDF6_VOICETABLE.SUBTITLE.SGO";
        Check(table.LoadFile(file, &error), error.empty() ? "real subtitle table load failed" : error.c_str());
        Check(table.Size() == 13869, "unexpected real table count");
        BasicAndFailure();
        NullOutputAndSurround();
        NestedAndRepeated();
        ConcurrentCaptures();
        std::cout << "PASS " << checks << " real Runtime hook integration checks; 128 concurrent successful requests\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "FAIL after " << checks << " checks: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
