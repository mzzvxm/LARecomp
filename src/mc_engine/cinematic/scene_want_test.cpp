// Check of the cell that holds which scene is asked for (scene_want.h, the value of mcla_scene): that a scene named at run time (a tour's
// `set mcla_scene`, the F4 panel, the console) reaches the runner, on a settings registry that behaves like 0.11's, where the setting
// does not exist before RegisterAll. A program of its own: it needs no game, no ReXGlue and no log, and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_scene_want_test      (CMakeLists.txt)
//   <build dir>/larecomp_scene_want_test                              the checks
//
// The compile by hand (the msvc-target clang of labuild, include path src/mc_engine/cinematic):
//   clang++ -std=c++23 src/mc_engine/cinematic/scene_want_test.cpp -o scene_want_test.exe

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "scene_want.h"

using mc::cinematic::SceneWant;

namespace {

int g_checks = 0;
int g_failed = 0;

void Check(bool ok, const std::string& what) {
    ++g_checks;
    if (!ok) {
        ++g_failed;
        std::fprintf(stderr, "FAILED: %s\n", what.c_str());
    }
}

void CheckEq(const std::string& got, const std::string& want, const std::string& what) {
    ++g_checks;
    if (got != want) {
        ++g_failed;
        std::fprintf(stderr, "FAILED: %s: got '%s', want '%s'\n", what.c_str(), got.c_str(), want.c_str());
    }
}

// The settings registry as the cell sees it (larecomp::cvar::RegisterChangeCallback and GetFlagByName), with 0.11's rule: a watch asked
// for before RegisterAll finds no setting and is refused. Set is SetFlagByName: stores, then runs the watches on the writer's thread.
struct FakeSetting {
    std::mutex mutex;
    bool registered = false;  // RegisterAll has run
    std::string text;
    std::vector<SceneWant::Notify> watches;
    int watch_calls = 0;
    int refused = 0;

    bool Watch(SceneWant::Notify notify) {
        std::lock_guard<std::mutex> lock(mutex);
        ++watch_calls;
        if (!registered) {
            ++refused;
            return false;
        }
        watches.push_back(std::move(notify));
        return true;
    }
    std::string Read() {
        std::lock_guard<std::mutex> lock(mutex);
        return text;
    }
    void Set(const std::string& value) {
        std::vector<SceneWant::Notify> run;
        {
            std::lock_guard<std::mutex> lock(mutex);
            text = value;
            run = watches;
        }
        for (const SceneWant::Notify& notify : run) notify(value);
    }
    void RegisterAll() {
        std::lock_guard<std::mutex> lock(mutex);
        registered = true;
    }
    SceneWant::Source Source() {
        return {[this](SceneWant::Notify notify) { return Watch(std::move(notify)); }, [this] { return Read(); }};
    }
};

// The way it was lost: the cell is built at static initialization (here: before RegisterAll), the setting is registered later, and a
// scene is named while the game runs.
void TestSetAtRunTime() {
    FakeSetting setting;
    SceneWant want(setting.Source());  // the namespace-scope object
    Check(setting.watch_calls == 0, "building the cell asks the registry for nothing (the setting does not exist yet)");
    setting.RegisterAll();

    CheckEq(want.Wanted(), "", "nothing asked for at the first tick");
    Check(!want.IsSet(), "no scene: not set");
    Check(want.Watched(), "the watch was hooked up on the first ask, after RegisterAll");
    Check(setting.watch_calls == 1, "one watch, hooked up once");

    setting.Set("C:\\game\\scenes-test\\rt-parts.scene");  // `set mcla_scene <file>`
    CheckEq(want.Wanted(), "C:\\game\\scenes-test\\rt-parts.scene", "a scene named at run time reaches the runner");
    Check(want.IsSet(), "and the camera hook's thread sees a scene pending");

    setting.Set("wing.scene");  // another scene
    CheckEq(want.Wanted(), "wing.scene", "another scene replaces it");

    setting.Set("");  // the scene handed back
    CheckEq(want.Wanted(), "", "cleared: off");
    Check(!want.IsSet(), "cleared: not set");
    Check(setting.watch_calls == 1, "later asks hook nothing up again");
}

// larecomp.toml or the command line: the value is there before anyone asks, no write through SetFlagByName follows.
void TestValueFromFile() {
    FakeSetting setting;
    SceneWant want(setting.Source());
    setting.RegisterAll();
    setting.text = "from-toml.scene";
    CheckEq(want.Wanted(), "from-toml.scene", "a value from the file is read at the first ask");
    Check(want.IsSet(), "and counts as pending");
    setting.Set("other.scene");
    CheckEq(want.Wanted(), "other.scene", "an edit after that follows");
}

// A write that lands between hooking the watch up and the first read: its callback holds the newer value, the read must not undo it.
void TestWriteDuringSeed() {
    FakeSetting setting;
    setting.RegisterAll();
    setting.text = "stale.scene";
    SceneWant::Source source;
    source.watch = [&](SceneWant::Notify notify) {
        const bool ok = setting.Watch(notify);
        notify("newer.scene");  // the writer, right after the watch exists
        return ok;
    };
    source.read = [&] { return std::string("stale.scene"); };  // the read that follows the write sees the old text
    SceneWant want(std::move(source));
    CheckEq(want.Wanted(), "newer.scene", "a change between the watch and the first read is kept");
}

// A refused watch must not cost the scene: the cell reads the setting on every ask instead.
void TestRefusedWatchPolls() {
    FakeSetting setting;  // never registered: every watch is refused
    SceneWant want(setting.Source());
    CheckEq(want.Wanted(), "", "refused watch: nothing asked for");
    Check(!want.Watched(), "refused watch: reported");
    Check(setting.refused == 1, "refused watch: the registry was asked once");
    setting.Set("polled.scene");
    CheckEq(want.Wanted(), "polled.scene", "refused watch: the new value is read all the same");
    Check(want.IsSet(), "refused watch: set");
    setting.Set("");
    Check(!want.IsSet(), "refused watch: cleared");
    CheckEq(want.Wanted(), "", "refused watch: cleared, game thread");
}

// The fake behaves like 0.11: a watch before RegisterAll is dropped and a later write never reaches it.
void TestFakeIsLikeZeroEleven() {
    FakeSetting setting;
    int told = 0;
    Check(!setting.Watch([&](std::string_view) { ++told; }), "the fake refuses a watch before RegisterAll");
    setting.RegisterAll();
    setting.Set("a.scene");
    Check(told == 0, "and a callback it refused is never run");
}

// One writer, the game thread (Wanted) and the camera hook's thread (IsSet) at once: the text is always one that was written.
void TestThreads() {
    FakeSetting setting;
    setting.RegisterAll();
    SceneWant want(setting.Source());
    const std::vector<std::string> values = {"", "one.scene", "two.scene", "a-longer-scene-name-that-does-not-fit-the-small-string.scene"};
    std::atomic<bool> stop{false};
    std::atomic<int> bad{0};
    std::thread camera([&] {
        while (!stop.load()) (void)want.IsSet();
    });
    std::thread writer([&] {
        for (int i = 0; i < 20000; ++i) setting.Set(values[static_cast<size_t>(i) % values.size()]);
        stop.store(true);
    });
    while (!stop.load()) {
        const std::string got = want.Wanted();
        bool known = false;
        for (const std::string& v : values) known = known || got == v;
        if (!known) ++bad;
    }
    writer.join();
    camera.join();
    Check(bad.load() == 0, "threads: the game thread never saw a torn or invented value");
    CheckEq(want.Wanted(), values[(20000 - 1) % values.size()], "threads: the last write is the one the game thread ends with");
}

// The cause was one line: a change callback added from a namespace-scope initializer. A call is a statement in a function and starts
// indented; a call at the start of a line (column 0) is a namespace-scope initializer. Looks at every source under src.
void TestNoStaticInitCallbacks() {
    namespace fs = std::filesystem;
    const fs::path src = fs::path(__FILE__).parent_path().parent_path().parent_path();
    std::error_code ec;
    if (!fs::is_directory(src / "mc_engine", ec)) {
        std::printf("scene_want_test: source check skipped (%s is not the source folder)\n", src.string().c_str());
        return;
    }
    int files = 0;
    int calls = 0;
    for (fs::recursive_directory_iterator it(src, fs::directory_options::skip_permission_denied, ec), end; it != end; it.increment(ec)) {
        if (ec) break;
        const fs::path& p = it->path();
        const std::string ext = p.extension().string();
        const std::string name = p.filename().string();
        if ((ext != ".cpp" && ext != ".h") || name.ends_with("_test.cpp")) continue;
        std::ifstream in(p);
        if (!in) continue;
        ++files;
        std::string line;
        int n = 0;
        while (std::getline(in, line)) {
            ++n;
            if (line.find("RegisterChangeCallback") == std::string::npos) continue;
            ++calls;
            if (line.empty() || line[0] == ' ' || line[0] == '\t' || line.rfind("//", 0) == 0) continue;
            if (line.rfind("bool RegisterChangeCallback", 0) == 0 || line.rfind("void RegisterChangeCallback", 0) == 0) continue;  // the definition and the declaration
            Check(false, "a change callback is added from a namespace-scope initializer (" + p.string() + ":" + std::to_string(n) +
                             "): the setting does not exist yet on 0.11, so the callback is dropped; add it on first use");
        }
    }
    Check(files > 50, "the source check read the source tree (" + std::to_string(files) + " files)");
    Check(calls > 10, "the source check found the callers (" + std::to_string(calls) + " lines)");
}

}  // namespace

int main() {
    TestFakeIsLikeZeroEleven();
    TestSetAtRunTime();
    TestValueFromFile();
    TestWriteDuringSeed();
    TestRefusedWatchPolls();
    TestThreads();
    TestNoStaticInitCallbacks();
    std::printf("scene_want_test: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
