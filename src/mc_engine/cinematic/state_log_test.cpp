// Check of the state log's pure half (state_log_core.h): the columns and rows, the hashes, the file's place, the bounded queue and
// the writer. A program of its own: it needs no game, no ReXGlue and no log, and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_state_log_test      (CMakeLists.txt)
//   <build dir>/larecomp_state_log_test                              the checks
//   <build dir>/larecomp_state_log_test --sample out.csv [rows [drift_row [drift_metres]]]
//                                                                    no checks: writes rows made up the way a parked car and a driven
//                                                                    one look, through the real formatter and writer, for
//                                                                    take_diff.py (the chain's unit step compares two of them)
//
// The compile by hand (the msvc-target clang of labuild, include path src/mc_engine/cinematic):
//   clang++ -std=c++23 -pthread src/mc_engine/cinematic/state_log_test.cpp src/mc_engine/cinematic/state_log_core.cpp -o state_log_test.exe

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "state_log_core.h"

using namespace mc::cinematic::statelog;

namespace {

int g_checks = 0;
int g_failed = 0;

void Check(bool ok, const std::string& what) {
    ++g_checks;
    if (!ok) {
        ++g_failed;
        std::printf("FAILED: %s\n", what.c_str());
    }
}

void CheckEq(const std::string& got, const std::string& want, const std::string& what) {
    ++g_checks;
    if (got != want) {
        ++g_failed;
        std::printf("FAILED: %s: got '%s', want '%s'\n", what.c_str(), got.c_str(), want.c_str());
    }
}

std::vector<std::string> Split(const std::string& line, char sep = ',') {
    std::vector<std::string> out;
    std::string cur;
    for (char c : line) {
        if (c == sep) {
            out.push_back(cur);
            cur.clear();
        } else if (c != '\n' && c != '\r') {
            cur += c;
        }
    }
    out.push_back(cur);
    return out;
}

// The value of column `name` in a row.
std::string Field(const std::string& row, const std::string& name) {
    const std::vector<std::string> names = ColumnNames();
    const std::vector<std::string> f = Split(row);
    for (size_t i = 0; i < names.size() && i < f.size(); ++i) {
        if (names[i] == name) return f[i];
    }
    return "<no such column>";
}

void SetId(CarSample& car, const char* id) {
    std::snprintf(car.id, sizeof car.id, "%s", id);
}

Sample Parked(uint64_t tick) {
    Sample s;
    s.tick = tick;
    s.wall_ms = 12.5 * static_cast<double>(tick);
    s.scene_state = 3;
    s.scene_t = static_cast<double>(tick) / 60.0;
    s.dt = 0.0166666675f;
    s.dt_unscaled = 0.0166666675f;
    s.time_scale = 1.0f;
    s.frame_ctr = static_cast<uint32_t>(1000 + tick);
    s.clock_total = static_cast<float>(tick) / 60.0f;
    s.clock_game = s.clock_total;
    s.mode_timer = 30.0f + s.clock_total;
    s.rng = 12345u + static_cast<uint32_t>(tick);
    s.hour = 17.4f;
    s.weather = 0;
    s.live = 1;
    s.p_ok = true;
    const float m[16] = {-0.880069196f, 0.0443580896f, -0.472769111f, 1e19f,   0.0215489343f, 0.998332322f, 0.0535558723f, -1.0e30f,
                         0.474356323f,  0.0369452015f, -0.879557371f,  0.0f,   819.610535f,   17.5570717f,  -808.407166f,  1.0f};
    std::memcpy(s.p_m, m, sizeof m);
    s.p_speed = 0.00022f;
    s.cam_ok = true;
    const float c[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 815.5f, 19.0f, -800.25f, 1};
    std::memcpy(s.cam, c, sizeof c);
    return s;
}

// ------------------------------------------------------------------------------------------------------------ the columns

void TestColumns() {
    const std::vector<std::string> names = ColumnNames();
    // 4 meta + 2 scene + 7 clock + 5 world + (1 + 12 + 1) player + (1 + 12) camera + 1 + 6 cars x 14 + 6 hashes
    const size_t want = 4 + 2 + 7 + 5 + 14 + 13 + 1 + 6 * 14 + 6;
    Check(names.size() == want, "the column count is " + std::to_string(want) + " (got " + std::to_string(names.size()) + ")");
    std::set<std::string> unique(names.begin(), names.end());
    Check(unique.size() == names.size(), "every column name is unique");
    Check(names.front() == "tick" && names[1] == "mark" && names[2] == "wall_ms", "the file starts tick, mark, wall_ms (take_diff.py's meta columns)");
    Check(names.back() == "chain" && names[names.size() - 2] == "hash", "the file ends hash, chain");
    for (const char* n : {"p_m0", "p_m12", "p_m13", "p_m14", "p_speed", "cam_m12", "c0_id", "c5_m14", "n_cars", "rng", "scene_t", "frame_ctr"}) {
        Check(unique.count(n) == 1, std::string("column ") + n + " exists");
    }
    for (const char* n : {"p_m3", "p_m7", "p_m11", "p_m15", "cam_m3", "c0_m3", "c0_m15"}) {
        Check(unique.count(n) == 0, std::string("the padding word ") + n + " is not logged");
    }
    CheckEq(ColumnLine().substr(0, 20), "tick,mark,wall_ms,lo", "the column line starts as the names do");
    const std::string column_line = ColumnLine();
    Check(std::count(column_line.begin(), column_line.end(), ',') + 1 == static_cast<long>(names.size()), "the column line has one field a name");

    Check(PoseWordIndex(0) == 0 && PoseWordIndex(2) == 2 && PoseWordIndex(3) == 4 && PoseWordIndex(5) == 6 && PoseWordIndex(6) == 8 &&
              PoseWordIndex(8) == 10 && PoseWordIndex(9) == 12 && PoseWordIndex(11) == 14,
          "the twelve words are the matrix cells without the fourth of each row");
}

void TestRow() {
    Chain chain;
    const Sample s = Parked(7);
    const std::string row = RowText(s, &chain);
    Check(!row.empty() && row.back() == '\n', "a row ends with a newline");
    const std::vector<std::string> f = Split(row);
    Check(f.size() == ColumnNames().size(), "a row has one field a column (" + std::to_string(f.size()) + ")");
    CheckEq(Field(row, "tick"), "7", "tick");
    CheckEq(Field(row, "scene_state"), "3", "scene_state");
    CheckEq(Field(row, "scene_t"), "0.116667", "scene_t is printed with six decimals");
    CheckEq(Field(row, "frame_ctr"), "1007", "frame_ctr");
    CheckEq(Field(row, "wall_ms"), "87.500", "wall_ms");
    CheckEq(Field(row, "p_ok"), "1", "p_ok");
    CheckEq(Field(row, "n_cars"), "0", "n_cars");
    CheckEq(Field(row, "c0_id"), "", "an empty car slot has an empty id");
    CheckEq(Field(row, "c0_ok"), "0", "an empty car slot is not ok");
    CheckEq(Field(row, "c3_m14"), "0", "an empty car slot's matrix is zeros");
    CheckEq(Field(row, "lost"), "0", "lost");

    // a float comes back exactly from its 9 digits
    for (int k = 0; k < kPoseWords; ++k) {
        const int i = PoseWordIndex(k);
        const std::string name = "p_m" + std::to_string(i);
        const float back = std::strtof(Field(row, name).c_str(), nullptr);
        Check(back == s.p_m[i], name + " reads back as the same float");
    }
    Check(std::strtof(Field(row, "p_speed").c_str(), nullptr) == s.p_speed, "p_speed reads back as the same float");
    Check(std::strtof(Field(row, "dt").c_str(), nullptr) == s.dt, "dt reads back as the same float");
    Check(std::strtof(Field(row, "cam_m14").c_str(), nullptr) == s.cam[14], "cam_m14 reads back as the same float");
    // the padding (1e19, -1e30) is in no field
    Check(row.find("1e+19") == std::string::npos && row.find("e+30") == std::string::npos, "the padding words are nowhere in the row");

    // cars
    Sample withCars = Parked(7);
    withCars.n_cars = 2;
    SetId(withCars.car[0], "p1");
    withCars.car[0].ok = true;
    withCars.car[0].m[12] = 817.0f;
    withCars.car[0].m[13] = 17.6f;
    withCars.car[0].m[14] = -797.3f;
    SetId(withCars.car[1], "a,b\n\"c\"");  // a hostile id
    Chain c2;
    const std::string r2 = RowText(withCars, &c2);
    Check(Split(r2).size() == ColumnNames().size(), "a row with cars and a hostile id keeps its field count");
    CheckEq(Field(r2, "c0_id"), "p1", "car id");
    CheckEq(Field(r2, "c0_ok"), "1", "car ok");
    Check(std::strtof(Field(r2, "c0_m12").c_str(), nullptr) == 817.0f, "car position x");
    CheckEq(Field(r2, "c1_id"), "a;b;;c;", "a comma, a line break and quotes in an id become ';'");
    CheckEq(Field(r2, "n_cars"), "2", "n_cars");

    // a mark with a comma
    Sample marked = Parked(7);
    marked.mark = "go,now";
    Chain c3;
    const std::string r3 = RowText(marked, &c3);
    CheckEq(Field(r3, "mark"), "go;now", "a mark's comma becomes ';'");
    Check(Split(r3).size() == ColumnNames().size(), "a row with a mark keeps its field count");

    // lost
    Sample lost = Parked(7);
    lost.lost = 12;
    Chain c4;
    CheckEq(Field(RowText(lost, &c4), "lost"), "12", "lost rows are in the row after them");
}

void TestHashes() {
    Chain a, b;
    const std::string ra = RowText(Parked(5), &a);
    // the same state with different clocks, wall time, tick, mark, generator and scene clock: the same hash
    Sample t = Parked(5);
    t.tick = 99;
    t.wall_ms = 5000;
    t.mark = "x";
    t.rng = 777;
    t.scene_t = 123.0;
    t.scene_state = 4;
    t.dt = 0.01f;
    t.dt_unscaled = 0.02f;
    t.time_scale = 0.5f;
    t.frame_ctr = 5;
    t.clock_total = 9;
    t.clock_game = 9;
    t.mode_timer = 9;
    t.lost = 3;
    t.p_m[3] = 5.0f;  // padding
    t.p_m[7] = 6.0f;
    const std::string rb = RowText(t, &b);
    CheckEq(Field(rb, "hash"), Field(ra, "hash"), "the hash leaves out the clocks, the generator, the scene clock, the wall time and the padding");
    CheckEq(Field(rb, "chain"), Field(ra, "chain"), "so the chains agree too");
    CheckEq(Field(rb, "h_player"), Field(ra, "h_player"), "and the player group");

    // 1e-7 m of a position changes the hash
    Sample p = Parked(5);
    p.p_m[12] = std::nextafter(p.p_m[12], 1e9f);
    Chain c;
    const std::string rp = RowText(p, &c);
    Check(Field(rp, "hash") != Field(ra, "hash"), "the next float of the player's x changes the hash");
    Check(Field(rp, "h_player") != Field(ra, "h_player") && Field(rp, "h_cam") == Field(ra, "h_cam") && Field(rp, "h_world") == Field(ra, "h_world"),
          "... and only the player group");
    Sample cam = Parked(5);
    cam.cam[12] += 0.001f;
    Chain c5;
    const std::string rc = RowText(cam, &c5);
    Check(Field(rc, "h_cam") != Field(ra, "h_cam") && Field(rc, "h_player") == Field(ra, "h_player"), "the camera has its own group");
    Sample hr = Parked(5);
    hr.hour = 17.5f;
    Chain c6;
    Check(Field(RowText(hr, &c6), "h_world") != Field(ra, "h_world"), "the hour is in the world group");
    Sample car = Parked(5);
    car.n_cars = 1;
    SetId(car.car[0], "p1");
    Chain c7;
    const std::string rcar = RowText(car, &c7);
    Check(Field(rcar, "h_cars") != Field(ra, "h_cars"), "a car's id and count are in the cars group");
    Sample ok1 = car;
    ok1.car[0].ok = true;
    Chain c8;
    Check(Field(RowText(ok1, &c8), "h_cars") != Field(rcar, "h_cars"), "a car's ok bit is in the cars group");

    // the chain depends on every row and their order
    Chain x, y;
    RowText(Parked(1), &x);
    const std::string x2 = RowText(Parked(2), &x);
    RowText(Parked(2), &y);
    const std::string y2 = RowText(Parked(1), &y);
    // Parked(1) and Parked(2) have the same state (a parked car): the hashes are equal row by row, so is the chain
    CheckEq(Field(x2, "chain"), Field(y2, "chain"), "two rows of one state in either order give one chain");
    Chain m, n;
    Sample moved = Parked(1);
    moved.p_m[12] += 1.0f;
    RowText(Parked(1), &m);
    RowText(moved, &m);
    RowText(moved, &n);
    const std::string n2 = RowText(Parked(1), &n);
    const std::string m2 = RowText(Parked(1), &m);
    Check(Field(n2, "chain") != Field(m2, "chain"), "a different order of different states gives another chain");
    Check(m.value != 0, "the chain advances");
    // a row without a chain argument works
    Check(!RowText(Parked(1), nullptr).empty(), "a row can be made without a chain");
}

void TestTextHelpers() {
    CheckEq(CleanField("ok name", 64), "ok name", "a plain field is kept");
    CheckEq(CleanField("a,b", 64), "a;b", "a comma becomes ';'");
    CheckEq(CleanField("a\"b\r\nc\tz", 64), "a;b;;c;z", "quotes and control characters become ';'");
    CheckEq(CleanField("abcdefgh", 4), "abcd", "a field is cut at its limit");

    CheckEq(CsvPathFor("C:\\takes\\one"), "C:\\takes\\one/state.csv", "a folder holds state.csv");
    CheckEq(CsvPathFor("C:\\takes\\one\\"), "C:\\takes\\one/state.csv", "a trailing slash is not doubled");
    CheckEq(CsvPathFor("C:/takes/one/"), "C:/takes/one/state.csv", "a trailing forward slash is not doubled");
    CheckEq(CsvPathFor("C:\\takes\\a.csv"), "C:\\takes\\a.csv", "a .csv path is a file");
    CheckEq(CsvPathFor("a.CSV"), "a.CSV", "any case of .csv");
    CheckEq(CsvPathFor("a.csv.d"), "a.csv.d/state.csv", ".csv in the middle is a folder name");
    CheckEq(CsvPathFor("x"), "x/state.csv", "a bare name is a folder");

    CheckEq(SafeFileStem("my scene"), "my_scene", "a space in a scene name");
    CheckEq(SafeFileStem("a/b\\c:d"), "a_b_c_d", "path characters in a scene name");
    CheckEq(SafeFileStem(""), "scene", "an empty name");
    CheckEq(SafeFileStem(".."), "scene", "dots only");
    CheckEq(SafeFileStem("intro-sunset_2.v1"), "intro-sunset_2.v1", "a good name is kept");

    CheckEq(ResolveLogPath("C:\\t\\a.csv", "D", "E", "s", "20261113-120000"), "C:\\t\\a.csv", "the cvar comes first (a file)");
    CheckEq(ResolveLogPath("C:\\t", "D", "E", "s", "20261113-120000"), "C:\\t/state.csv", "the cvar comes first (a folder)");
    CheckEq(ResolveLogPath("", "D:\\x", "E", "s", "20261113-120000"), "D:\\x/state.csv", "the scene's dir is second");
    CheckEq(ResolveLogPath("", "", "E:\\take", "s", "20261113-120000"), "E:\\take/state.csv", "the take folder is third");
    CheckEq(ResolveLogPath("", "", "", "intro sunset", "20261113-120000"), "statelogs/intro_sunset-20261113-120000.csv",
            "else statelogs/<scene>-<stamp>.csv next to the game");
    CheckEq(ResolveLogPath("", "", "", "", ""), "statelogs/scene-now.csv", "no scene name, no stamp");

    auto exists_none = [](const std::string&) { return false; };
    CheckEq(FirstFreePath("a/state.csv", exists_none), "a/state.csv", "a free name is used");
    std::set<std::string> taken = {"a/state.csv", "a/state-2.csv"};
    auto exists = [&](const std::string& p) { return taken.count(p) != 0; };
    CheckEq(FirstFreePath("a/state.csv", exists), "a/state-3.csv", "the first free number");
    CheckEq(FirstFreePath("dir.d/noext", [](const std::string& p) { return p == "dir.d/noext"; }), "dir.d/noext-2", "a dot in a folder name is not an extension");
    CheckEq(FirstFreePath("a/state.csv", [](const std::string&) { return true; }), "a/state.csv", "a thousand takes: the first name again");
}

// ------------------------------------------------------------------------------------------------------------ queue and writer

void TestQueue() {
    RowQueue q(100);
    Check(q.Push(std::string(40, 'a')), "a row under the cap is taken");
    Check(q.Push(std::string(40, 'b')), "a second one too");
    Check(!q.Push(std::string(40, 'c')), "a row that would pass the cap is refused");
    Check(q.pending_bytes() == 80 && q.pushed() == 2 && q.dropped() == 1, "80 bytes pending, 2 pushed, 1 dropped");
    std::string out = "x";
    Check(q.PopAll(&out) == 80, "PopAll moves 80 bytes");
    Check(out == "x" + std::string(40, 'a') + std::string(40, 'b'), "the rows come out in order, appended");
    Check(q.pending_bytes() == 0, "nothing pending after PopAll");
    Check(q.Push(std::string(100, 'd')), "after a pop there is room again (exactly the cap fits)");
    Check(!q.Push(std::string(101, 'e')), "a row larger than the cap never fits");
    q.Reset();
    Check(q.pending_bytes() == 0 && q.pushed() == 0 && q.dropped() == 0, "Reset forgets everything");
    // many threads
    RowQueue big(1u << 20);
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&big] {
            for (int i = 0; i < 1000; ++i) big.Push("0123456789\n");
        });
    }
    for (auto& t : threads) t.join();
    Check(big.pushed() == 4000 && big.dropped() == 0 && big.pending_bytes() == 44000, "4 threads, 4000 rows, none lost");
}

std::string Slurp(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void TestWriter() {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / ("larecomp_state_log_test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root);
    const std::string path = (root / "deep" / "er" / "state.csv").string();

    {
        LogWriter w(1u << 20, 5);
        Check(!w.IsOpen(), "not open before Open");
        Check(!w.Push("row\n"), "a row pushed to a closed writer is refused");
        Check(w.Open(path, PreambleLine("intro", 0, "20261113-120000") + ColumnLine() + "\n"), "Open creates the folders and the file");
        Check(w.IsOpen(), "open");
        Check(!w.Open(path, "x"), "a second Open on an open writer is refused");
        Chain chain;
        for (uint64_t i = 0; i < 500; ++i) w.Push(RowText(Parked(i), &chain));
        std::this_thread::sleep_for(std::chrono::milliseconds(40));  // the thread flushes by itself
        const std::string midway = Slurp(path);
        Check(midway.find("#statelog v2") == 0, "the head is written at Open");
        Check(std::count(midway.begin(), midway.end(), '\n') > 3, "the thread has written rows before Close");
        w.Close(500);
        Check(!w.IsOpen(), "closed");
        Check(w.written() == 500 && w.dropped() == 0, "500 rows written, none dropped");
        w.Close(500);  // a second Close does nothing
    }
    const std::string text = Slurp(path);
    std::istringstream in(text);
    std::string line;
    std::vector<std::string> lines;
    while (std::getline(in, line)) lines.push_back(line);
    Check(lines.size() == 1 + 1 + 500 + 1, "preamble, columns, 500 rows and the trailer (" + std::to_string(lines.size()) + " lines)");
    Check(!lines.empty() && lines[0].rfind("#statelog v2 scene=intro started=20261113-120000", 0) == 0, "the first line names the format and the scene");
    Check(lines.size() > 1 && lines[1] == ColumnLine(), "the second line is the column line");
    Check(lines.size() > 2 && Field(lines[2] + "\n", "tick") == "0" && Field(lines[501] + "\n", "tick") == "499", "rows in order from tick 0 to 499");
    Check(!lines.empty() && lines.back() == "#end rows=500 written=500 dropped=0", "the trailer counts the rows (got '" + (lines.empty() ? std::string() : lines.back()) + "')");

    // a cap smaller than the rows: rows are dropped and counted, the file stays well formed
    const std::string small = (root / "small.csv").string();
    {
        LogWriter w(3000, 1000);  // a slow thread: the queue fills first
        Check(w.Open(small, ColumnLine() + "\n"), "Open (small cap)");
        Chain chain;
        int refused = 0;
        for (uint64_t i = 0; i < 100; ++i) refused += w.Push(RowText(Parked(i), &chain)) ? 0 : 1;
        Check(refused > 0 && static_cast<uint64_t>(refused) == w.dropped(), "rows past the cap are refused and counted");
        Check(w.written() + w.dropped() == 100, "taken + dropped = pushed");
        const uint64_t taken = w.written();
        w.Close(100);
        const std::string t2 = Slurp(small);
        Check(static_cast<uint64_t>(std::count(t2.begin(), t2.end(), '\n')) == 1 + taken + 1, "the file holds the head, the rows taken and the trailer");
        Check(t2.find("#end rows=100 written=" + std::to_string(taken) + " dropped=" + std::to_string(refused)) != std::string::npos,
              "the trailer says how many were dropped");
    }

    // a path that cannot be made
    {
        LogWriter w;
        const std::string bad = (root / "small.csv" / "x" / "state.csv").string();  // a file where a folder should be
        Check(!w.Open(bad, "x"), "Open fails when a folder cannot be made");
        Check(!w.IsOpen(), "a failed Open leaves it closed");
        // and it can open a good one after that
        Check(w.Open((root / "after.csv").string(), "head\n"), "a writer that failed once can open another file");
        w.Close(0);
        Check(Slurp(root / "after.csv") == "head\n#end rows=0 written=0 dropped=0\n", "an empty log is its head and trailer");
    }
    // the destructor closes an open log
    {
        const std::string p2 = (root / "dtor.csv").string();
        {
            LogWriter w;
            w.Open(p2, "head\n");
            w.Push("r1\n");
        }
        Check(Slurp(root / "dtor.csv") == "head\nr1\n#end rows=1 written=1 dropped=0\n", "the destructor flushes and closes");
    }
    // reopened: counts start again
    {
        LogWriter w(100, 5);
        w.Open((root / "re1.csv").string(), "h\n");
        for (int i = 0; i < 20; ++i) w.Push("0123456789\n");
        w.Close(20);
        const uint64_t dropped1 = w.dropped();
        Check(dropped1 > 0, "the first log dropped rows (the cap is 100 bytes)");
        w.Open((root / "re2.csv").string(), "h\n");
        Check(w.written() == 0 && w.dropped() == 0, "a second log starts its counts again");
        w.Push("a\n");
        w.Close(1);
        Check(Slurp(root / "re2.csv") == "h\na\n#end rows=1 written=1 dropped=0\n", "the second file is its own");
    }
    std::error_code ec;
    fs::remove_all(root, ec);
}

void TestPreamble() {
    const std::string p = PreambleLine("intro sunset,x", 60.0, "20261113-120000");
    Check(p.rfind("#statelog v2 scene=intro sunset;x started=20261113-120000 fixed_step_fps=60 step_ticks=831250 tick_seconds=", 0) == 0,
          "the preamble: format, scene, start, the fixed step in the game's ticks (got " + p + ")");
    Check(p.back() == '\n', "the preamble ends with a newline");
    const std::string q = PreambleLine("", 0, "");
    Check(q.find("scene=- started=- fixed_step_fps=0 step_ticks=0 ") != std::string::npos, "no scene, no step: dashes and zeros");
    CheckEq(TrailerLine(10, 8, 2), "#end rows=10 written=8 dropped=2\n", "the trailer");
}

// ------------------------------------------------------------------------------------------------------------ --sample

// Writes a made-up log through the real formatter and writer: a parked car (a few micrometres of jitter, as E1b measured), or from
// `drift_row` on a car that has moved by `drift_metres` in x. Two scene cars, a mark at row 30.
int WriteSample(const std::string& path, int rows, int drift_row, double drift_metres) {
    LogWriter w;
    if (!w.Open(path, PreambleLine("sample", 60.0, "20261113-120000") + ColumnLine() + "\n")) {
        std::printf("cannot open %s\n", path.c_str());
        return 1;
    }
    Chain chain;
    for (int i = 0; i < rows; ++i) {
        Sample s = Parked(static_cast<uint64_t>(i));
        if (i == 30) s.mark = "go";
        s.n_cars = 2;
        SetId(s.car[0], "p1");
        SetId(s.car[1], "p2");
        for (int k = 0; k < 2; ++k) {
            s.car[k].ok = true;
            const float base[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 817.0f + 3.0f * static_cast<float>(k), 17.6f, -797.3f, 1};
            std::memcpy(s.car[k].m, base, sizeof base);
        }
        if (drift_row >= 0 && i >= drift_row) s.p_m[12] += static_cast<float>(drift_metres);
        w.Push(RowText(s, &chain));
    }
    w.Close(static_cast<uint64_t>(rows));
    std::printf("%s: %d rows\n", path.c_str(), rows);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 2 && std::strcmp(argv[1], "--sample") == 0) {
        const int rows = argc > 3 ? std::atoi(argv[3]) : 300;
        const int drift_row = argc > 4 ? std::atoi(argv[4]) : -1;
        const double drift = argc > 5 ? std::atof(argv[5]) : 0.0;
        return WriteSample(argv[2], rows, drift_row, drift);
    }
    TestColumns();
    TestRow();
    TestHashes();
    TestTextHelpers();
    TestQueue();
    TestWriter();
    TestPreamble();
    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
