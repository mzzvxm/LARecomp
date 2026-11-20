// The Director panel's logic, the pure half: see director_panel_core.h.

#include "director_panel_core.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <set>

#include "director_text.h"
#include "mc_engine/cinematic/director/director_math.h"

namespace mcla::ui::director {

namespace {

constexpr double kPi = 3.14159265358979323846;

bool ParseNumber(const std::string& s, double* out) {
    if (s.empty()) return false;
    char* end = nullptr;
    const double v = std::strtod(s.c_str(), &end);
    if (end != s.c_str() + s.size() || !std::isfinite(v)) return false;
    *out = v;
    return true;
}

dir::Json Vec3Json(const double v[3]) {
    return dir::Json::Vec3(v[0], v[1], v[2]);
}

// The comment of a line (from its '#', with the blank before it), or "".
std::string TrailingComment(const std::string& line) {
    bool quoted = false;
    for (size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (c == '"') quoted = !quoted;
        if (c == '#' && !quoted && (i == 0 || line[i - 1] == ' ' || line[i - 1] == '\t')) {
            size_t from = i;
            while (from > 0 && (line[from - 1] == ' ' || line[from - 1] == '\t')) --from;
            return line.substr(from);
        }
    }
    return {};
}

Cmd Make(const char* name) {
    Cmd c;
    c.cmd = name;
    return c;
}

}  // namespace

std::string FormatCmd(const Cmd& cmd) {
    return dir::FormatConsoleLine(cmd.cmd, cmd.args);
}

// ---------------------------------------------------------------------------------------------------------------- the dispatcher

struct Dispatcher::Shared {
    std::mutex mutex;
    uint64_t next_token = 1;
    struct Item {
        uint64_t token = 0;
        std::string cmd;
        Continuation then;
        dir::Reply reply;
        bool arrived = false;
    };
    std::vector<Item> items;  // sent, in order; an item is dropped from here when its continuation has run
};

Dispatcher::Dispatcher(PostFn post) : post_(std::move(post)), shared_(std::make_shared<Shared>()) {}

bool Dispatcher::Send(const Cmd& cmd, Continuation then) {
    uint64_t token = 0;
    {
        std::lock_guard<std::mutex> lock(shared_->mutex);
        token = shared_->next_token++;
        Shared::Item item;
        item.token = token;
        item.cmd = cmd.cmd;
        item.then = std::move(then);
        shared_->items.push_back(std::move(item));
    }
    dir::Posted p;
    p.cmd = cmd.cmd;
    p.args = cmd.args;
    p.source = dir::Source::kPanel;
    p.wait_settled = cmd.wait_settled;
    p.timeout_s = cmd.timeout_s;
    std::shared_ptr<Shared> shared = shared_;
    p.on_reply = [shared, token](const dir::Reply& reply) {
        std::lock_guard<std::mutex> lock(shared->mutex);
        for (Shared::Item& it : shared->items) {
            if (it.token == token) {
                it.reply = reply;
                it.arrived = true;
                break;
            }
        }
    };
    ++sent_;
    if (post_ && post_(std::move(p))) return true;
    {
        std::lock_guard<std::mutex> lock(shared_->mutex);
        shared_->items.erase(std::remove_if(shared_->items.begin(), shared_->items.end(), [&](const Shared::Item& it) { return it.token == token; }), shared_->items.end());
    }
    Note(last_now_, true, cmd.cmd + ": the Director did not take it (it is off, or its queue is full)");
    return false;
}

void Dispatcher::Deliver(uint64_t token, const dir::Reply& reply) {
    std::lock_guard<std::mutex> lock(shared_->mutex);
    for (Shared::Item& it : shared_->items) {
        if (it.token == token) {
            it.reply = reply;
            it.arrived = true;
            return;
        }
    }
}

void Dispatcher::Drain(double now) {
    last_now_ = now;
    std::vector<Shared::Item> done;
    {
        std::lock_guard<std::mutex> lock(shared_->mutex);
        // every reply that is in, in the order the commands were sent (a command that waits for the world to settle does not hold the others back)
        for (auto it = shared_->items.begin(); it != shared_->items.end();) {
            if (it->arrived) {
                done.push_back(std::move(*it));
                it = shared_->items.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (Shared::Item& it : done) {
        if (!it.reply.ok) Note(now, true, it.cmd + ": " + dir::ErrorCodeName(it.reply.code) + ": " + it.reply.message);
        if (it.then) it.then(it.reply);
    }
}

size_t Dispatcher::pending() const {
    std::lock_guard<std::mutex> lock(shared_->mutex);
    return shared_->items.size();
}

void Dispatcher::Note(double now, bool error, std::string text) {
    notices_.push_back({now, error, std::move(text)});
    while (notices_.size() > 8) notices_.erase(notices_.begin());
}

// ---------------------------------------------------------------------------------------------------------------- the scene's text

std::vector<std::string> SplitLines(const std::string& text) {
    std::vector<std::string> lines;
    size_t i = 0;
    while (i <= text.size()) {
        const size_t nl = text.find('\n', i);
        std::string line = text.substr(i, nl == std::string::npos ? std::string::npos : nl - i);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(std::move(line));
        if (nl == std::string::npos) break;
        i = nl + 1;
    }
    if (!lines.empty() && lines.back().empty()) lines.pop_back();  // the text's final newline is not a line
    return lines;
}

void SceneText::Set(int64_t rev, std::string text) {
    rev_ = rev;
    lines_ = SplitLines(text);
}

std::string SceneText::Line(int n) const {
    if (n < 1 || static_cast<size_t>(n) > lines_.size()) return {};
    return lines_[static_cast<size_t>(n) - 1];
}

std::string EditContext::ExpectFor(int line, const std::string& statement) const {
    if (text && text->Has() && text->rev() == rev) {
        const std::string full = text->Line(line);
        if (!full.empty()) return full;
    }
    return statement;
}

// ---------------------------------------------------------------------------------------------------------------- edit builders

Cmd EditCmd(int64_t base_rev, const std::vector<dir::EditOp>& ops) {
    Cmd c = Make("scene.edit");
    c.args.Set("base_rev", base_rev);
    dir::Json arr = dir::Json::Array();
    for (const dir::EditOp& op : ops) arr.Push(dir::EditOpToJson(op));
    c.args.Set("ops", std::move(arr));
    return c;
}

dir::EditOp OpMove(int line, const std::string& expect, double t) {
    dir::EditOp op;
    op.kind = dir::EditKind::kMove;
    op.line = line;
    op.expect = expect;
    op.has_t = true;
    op.t = t;
    return op;
}
dir::EditOp OpParam(int line, const std::string& expect, const std::string& key, const std::string& value) {
    dir::EditOp op;
    op.kind = dir::EditKind::kParam;
    op.line = line;
    op.expect = expect;
    op.key = key;
    op.value = value;
    return op;
}
dir::EditOp OpReplace(int line, const std::string& expect, const std::string& text) {
    dir::EditOp op;
    op.kind = dir::EditKind::kReplace;
    op.line = line;
    op.expect = expect;
    op.text = text;
    return op;
}
dir::EditOp OpDelete(int line, const std::string& expect) {
    dir::EditOp op;
    op.kind = dir::EditKind::kDelete;
    op.line = line;
    op.expect = expect;
    return op;
}
dir::EditOp OpInsert(const std::string& text) {
    dir::EditOp op;
    op.kind = dir::EditKind::kInsert;
    op.text = text;
    return op;
}

std::string StatementAtTime(const std::string& line, double t) {
    const std::string body = dir::StripComment(line);
    const dir::StatementInfo st = dir::ReadStatement(body);
    if (!st.has_time) return body;
    return body.substr(0, st.time_begin) + dir::FormatSceneTime(t) + body.substr(st.time_end);
}

// ---------------------------------------------------------------------------------------------------------------- keys

Cmd MoveKey(const EditContext& ctx, const Marker& m, double t) {
    return EditCmd(ctx.rev, {OpMove(m.line, ctx.ExpectFor(m.line, m.text), t)});
}

Cmd DuplicateKey(const EditContext& ctx, const Marker& m, double t) {
    const std::string line = ctx.ExpectFor(m.line, m.text);
    return EditCmd(ctx.rev, {OpInsert(StatementAtTime(line, t))});
}

Cmd DeleteKey(const EditContext& ctx, const Marker& m) {
    return EditCmd(ctx.rev, {OpDelete(m.line, ctx.ExpectFor(m.line, m.text))});
}

Cmd SetEase(const EditContext& ctx, const Marker& m, const std::string& ease) {
    return EditCmd(ctx.rev, {OpParam(m.line, ctx.ExpectFor(m.line, m.text), "ease", ease)});
}

Cmd SetKeyWord(const EditContext& ctx, const Marker& m, const std::string& key, const std::string& value) {
    return EditCmd(ctx.rev, {OpParam(m.line, ctx.ExpectFor(m.line, m.text), key, value)});
}

Cmd AddKeyHere(double t, bool use_t, const std::string& ease, bool cut) {
    Cmd c = Make("camera.key.add");
    if (use_t) c.args.Set("t", t);
    if (!ease.empty()) c.args.Set("ease", ease);
    if (cut) c.args.Set("cut", true);
    return c;
}

Cmd UpdateKeyFromPose(const EditContext& ctx, const Marker& m, const dir::SnapPose& pose) {
    const std::string line = ctx.ExpectFor(m.line, m.text);
    std::string text = "cam " + dir::FormatSceneTime(m.t) + " pos " + Num(pose.pos[0]) + " " + Num(pose.pos[1]) + " " + Num(pose.pos[2]) + " yaw " + Num(pose.yaw) + " pitch " + Num(pose.pitch) + " roll " +
                       Num(pose.roll) + " fov " + Num(pose.fov);
    if (!m.ease.empty()) text += " ease " + m.ease;
    text += TrailingComment(line);
    return EditCmd(ctx.rev, {OpReplace(m.line, line, text)});
}

Cmd CutAt(int64_t rev, double t) {
    return EditCmd(rev, {OpInsert("cut " + dir::FormatSceneTime(t))});
}

bool ParseKeyPose(const std::string& statement, dir::SnapPose* pose) {
    const std::vector<std::string> w = Words(statement);
    if (w.empty() || w[0] != "cam") return false;
    const int p = FindWord(w, "pos");
    if (p < 0 || static_cast<size_t>(p) + 3 >= w.size()) return false;
    dir::SnapPose out;
    for (int i = 0; i < 3; ++i) {
        if (!ParseNumber(w[static_cast<size_t>(p) + 1 + static_cast<size_t>(i)], &out.pos[i])) return false;
    }
    if (FindWord(w, "attach") >= 0 || FindWord(w, "orbit") >= 0 || FindWord(w, "rig") >= 0 || FindWord(w, "off") >= 0) return false;
    const int look = FindWord(w, "look");
    double v = 0.0;
    if (look >= 0) {
        double to[3];
        if (static_cast<size_t>(look) + 3 >= w.size()) return false;
        for (int i = 0; i < 3; ++i) {
            if (!ParseNumber(w[static_cast<size_t>(look) + 1 + static_cast<size_t>(i)], &to[i])) return false;  // `look car:p1 ...` depends on the car
        }
        dir::LookAtAngles(out.pos, to, &out.yaw, &out.pitch);
    } else {
        const bool have_yaw = ParseNumber(ValueAfter(w, "yaw"), &out.yaw);
        const bool have_pitch = ParseNumber(ValueAfter(w, "pitch"), &out.pitch);
        if (!have_yaw && !have_pitch) return false;
    }
    if (ParseNumber(ValueAfter(w, "roll"), &v)) out.roll = v;
    if (ParseNumber(ValueAfter(w, "fov"), &v)) out.fov = v;
    *pose = out;
    return true;
}

std::vector<KeyRef> CameraKeys(const std::vector<dir::SnapEvent>& events) {
    std::vector<KeyRef> keys;
    for (const dir::SnapEvent& e : events) {
        if (e.kind != dir::EventKind::kCamera) continue;
        if (FindWord(Words(e.text), "off") >= 0) continue;  // `cam T off` hands the camera back: it is not a place to go to
        keys.push_back({e.line, e.t, e.text});
    }
    std::stable_sort(keys.begin(), keys.end(), [](const KeyRef& a, const KeyRef& b) { return a.t < b.t; });
    return keys;
}

std::vector<Cmd> GoToKeyCmds(const dir::Snapshot& snap, int direction) {
    std::vector<Cmd> out;
    const std::vector<KeyRef> keys = CameraKeys(snap.events);
    const double now = snap.transport.t;
    const KeyRef* pick = nullptr;
    if (direction > 0) {
        for (const KeyRef& k : keys) {
            if (k.t > now + 1e-3) {
                pick = &k;
                break;
            }
        }
    } else {
        for (const KeyRef& k : keys) {
            if (k.t < now - 1e-3) pick = &k;
        }
    }
    if (!pick) return out;
    Cmd sel = Make("ui.select");
    sel.args.Set("line", pick->line);
    out.push_back(std::move(sel));
    out.push_back(SeekCmd(pick->t));
    dir::SnapPose pose;
    if (ParseKeyPose(pick->text, &pose)) out.push_back(CameraSetCmd(pose));
    else out.push_back(CameraModeCmd("scene"));
    return out;
}

// ---------------------------------------------------------------------------------------------------------------- rigs

const std::vector<std::string>& RigKinds() {
    static const std::vector<std::string> kinds = {"chase", "orbit", "tripod", "flyby", "hood", "bumper", "roof"};
    return kinds;
}

const std::vector<RigKnob>& KnobsFor(const std::string& kind) {
    static const std::vector<RigKnob> chase = {{"dist", "DISTANCE", 1.0, 30.0, 6.0, 1}, {"height", "HEIGHT", 0.0, 10.0, 1.8, 1}, {"side", "SIDE", -8.0, 8.0, 0.0, 1},
                                               {"lag", "LAG", 0.0, 2.0, 0.3, 2},       {"aim", "AIM HEIGHT", 0.0, 4.0, 1.0, 1}, {"ahead", "LOOK AHEAD", 0.0, 20.0, 0.0, 1}};
    static const std::vector<RigKnob> orbit = {{"r", "RADIUS", 2.0, 40.0, 8.0, 1}, {"el", "ELEVATION", -10.0, 80.0, 10.0, 0}, {"lag", "LAG", 0.0, 2.0, 0.0, 2}};
    static const std::vector<RigKnob> tripod = {{"aimlag", "AIM LAG", 0.0, 3.0, 0.0, 2}};
    static const std::vector<RigKnob> flyby = {{"up", "HEIGHT OVER ROUTE", 0.0, 30.0, 2.0, 1}, {"aimlag", "AIM LAG", 0.0, 3.0, 0.0, 2}};
    static const std::vector<RigKnob> mount = {{"ahead", "LOOK AHEAD", 0.0, 80.0, 30.0, 0}, {"lag", "LAG", 0.0, 1.0, 0.0, 2}};
    static const std::vector<RigKnob> none;
    if (kind == "chase") return chase;
    if (kind == "orbit") return orbit;
    if (kind == "tripod") return tripod;
    if (kind == "flyby") return flyby;
    if (kind == "hood" || kind == "bumper" || kind == "roof") return mount;
    return none;
}

const std::vector<RigKnob>& KeyKnobs() {
    static const std::vector<RigKnob> knobs = {{"fov", "FIELD OF VIEW", 10.0, 110.0, 50.0, 0}, {"roll", "ROLL", -45.0, 45.0, 0.0, 1}};
    return knobs;
}

bool ParseRig(const std::string& statement, RigInfo* out) {
    const std::vector<std::string> w = Words(statement);
    if (w.empty() || w[0] != "cam") return false;
    const int r = FindWord(w, "rig");
    if (r < 0 || static_cast<size_t>(r) + 1 >= w.size()) return false;
    RigInfo info;
    info.kind = w[static_cast<size_t>(r) + 1];
    for (const std::string& s : w) {
        if (s.rfind("car:", 0) == 0 && s.size() > 4) {
            info.subject = s.substr(4);
            break;
        }
    }
    for (const char* key : {"dist", "height", "side", "lag", "aim", "ahead", "r", "el", "aimlag", "up", "speed", "fov", "roll", "over"}) {
        double v = 0.0;
        if (ParseNumber(ValueAfter(w, key), &v)) info.values[key] = v;
    }
    info.has_over = info.values.count("over") > 0;
    const std::string az = ValueAfter(w, "az");
    if (!az.empty()) {
        const size_t dots = az.find("..");
        if (dots != std::string::npos) {
            info.az_range = ParseNumber(az.substr(0, dots), &info.az_from) && ParseNumber(az.substr(dots + 2), &info.az_to);
        } else if (ParseNumber(az, &info.az_from)) {
            info.az_to = info.az_from;
        }
    }
    *out = std::move(info);
    return true;
}

double KnobValue(const RigInfo& rig, const RigKnob& knob) {
    const auto it = rig.values.find(knob.key);
    return it == rig.values.end() ? knob.def : it->second;
}

double KeyKnobValue(const std::string& statement, const RigKnob& knob) {
    double v = 0.0;
    return ParseNumber(ValueAfter(Words(statement), knob.key), &v) ? v : knob.def;
}

std::string NewRigWords(const NewRig& rig) {
    const std::string car = rig.car.empty() ? "player" : rig.car;
    const std::string subject = "car:" + car;
    if (rig.kind == "chase") return "chase " + subject + " dist 6 height 1.8 lag 0.3";
    if (rig.kind == "orbit") return "orbit " + subject + " r 8 el 10 az 20..200 over 8";
    if (rig.kind == "tripod") {
        const std::string at = rig.has_pos ? Num(rig.pos[0]) + " " + Num(rig.pos[1]) + " " + Num(rig.pos[2]) : "0 2 0";
        return "tripod at " + at + " track " + subject;
    }
    if (rig.kind == "flyby") return "flyby path " + (rig.route.empty() ? std::string("run.route") : rig.route) + " track " + subject;
    return rig.kind + " " + subject;  // hood, bumper, roof
}

Cmd AddRigKey(double t, bool use_t, const NewRig& rig) {
    Cmd c = Make("camera.key.add");
    if (use_t) c.args.Set("t", t);
    c.args.Set("rig", NewRigWords(rig));
    return c;
}

Cmd SetKnob(const EditContext& ctx, const Marker& m, const RigKnob& knob, double value) {
    value = std::clamp(value, knob.lo, knob.hi);
    return EditCmd(ctx.rev, {OpParam(m.line, ctx.ExpectFor(m.line, m.text), knob.key, Num(value))});
}

Cmd SetOrbitAz(const EditContext& ctx, const Marker& m, const RigInfo& rig, double from, double to) {
    const std::string line = ctx.ExpectFor(m.line, m.text);
    const bool sweep = std::fabs(to - from) > 0.5;
    std::vector<std::pair<std::string, std::string>> sets = {{"az", sweep ? Num(from) + ".." + Num(to) : Num(from)}};
    if (sweep && !rig.has_over) sets.push_back({"over", "8"});  // a range needs the seconds the sweep takes
    return EditCmd(ctx.rev, {OpReplace(m.line, line, RewriteWords(line, sets))});
}

std::string RewriteWords(const std::string& line, const std::vector<std::pair<std::string, std::string>>& sets) {
    std::vector<std::string> words = Words(line);
    for (const auto& kv : sets) {
        const int i = FindWord(words, kv.first);
        if (i >= 0 && static_cast<size_t>(i) + 1 < words.size()) {
            words[static_cast<size_t>(i) + 1] = kv.second;
        } else if (i >= 0) {
            words.push_back(kv.second);
        } else {
            words.push_back(kv.first);
            words.push_back(kv.second);
        }
    }
    std::string out;
    for (const std::string& w : words) {
        if (!out.empty()) out += " ";
        out += w;
    }
    return out + TrailingComment(line);
}

// ---------------------------------------------------------------------------------------------------------------- cars

std::string NextCarId(const std::vector<dir::SnapCar>& cars, const std::vector<dir::SnapEvent>& events) {
    std::set<std::string> taken;
    for (const dir::SnapCar& c : cars) taken.insert(c.id);
    for (const dir::SnapEvent& e : events) {
        if (!e.car.empty()) taken.insert(e.car);
    }
    for (int i = 1; i < 10000; ++i) {
        const std::string id = "p" + std::to_string(i);
        if (!taken.count(id)) return id;
    }
    return "p10000";
}

Cmd SpawnCar(const std::string& id, const std::string& model, const double at[3], double heading, double t, int paint) {
    Cmd c = Make("car.spawn");
    c.args.Set("id", id);
    c.args.Set("model", model);
    c.args.Set("at", Vec3Json(at));
    c.args.Set("heading", std::round(heading * 10.0) / 10.0);
    if (t >= 0.0) c.args.Set("t", t);
    if (paint >= 0) c.args.Set("paint", paint);
    return c;
}

Cmd PickAt(double u, double v) {
    Cmd c = Make("query.pick");
    c.args.Set("u", u);
    c.args.Set("v", v);
    return c;
}

bool ParsePick(const dir::Reply& reply, double pos[3], std::string* why) {
    if (!reply.ok) {
        if (why) *why = reply.message;
        return false;
    }
    if (!reply.result.BoolOr("hit", false)) {
        if (why) *why = "nothing under the crosshair: aim the camera at the ground";
        return false;
    }
    if (!reply.result.Get("pos").ToVec3(pos)) {
        if (why) *why = "the pick has no position";
        return false;
    }
    return true;
}

int SpawnLineOf(const std::vector<dir::SnapEvent>& events, const std::string& car) {
    int line = -1;
    for (const dir::SnapEvent& e : events) {
        if (e.kind != dir::EventKind::kCar || e.car != car) continue;
        const std::string d = DescribeStatement(e);
        if (d.rfind("spawn", 0) == 0 || d.rfind("ambient", 0) == 0) line = e.line;
    }
    return line;
}

std::vector<int> LinesOfCar(const std::vector<dir::SnapEvent>& events, const std::string& car) {
    std::vector<int> lines;
    for (const dir::SnapEvent& e : events) {
        if (e.kind == dir::EventKind::kCar && e.car == car) lines.push_back(e.line);
    }
    return lines;
}

namespace {
const dir::SnapEvent* EventAt(const std::vector<dir::SnapEvent>& events, int line) {
    for (const dir::SnapEvent& e : events) {
        if (e.line == line) return &e;
    }
    return nullptr;
}
}  // namespace

Cmd MoveCarSpawn(const EditContext& ctx, const std::vector<dir::SnapEvent>& events, const std::string& car, const double at[3], double heading, bool set_heading) {
    const int line = SpawnLineOf(events, car);
    const dir::SnapEvent* e = EventAt(events, line);
    if (!e) return Cmd{};
    const std::string expect = ctx.ExpectFor(line, e->text);
    const std::string place = Num(at[0]) + "," + Num(at[1]) + "," + Num(at[2]);
    if (!set_heading) return EditCmd(ctx.rev, {OpParam(line, expect, "at", place)});
    const double h = std::fmod(std::fmod(heading, 360.0) + 360.0, 360.0);
    return EditCmd(ctx.rev, {OpReplace(line, expect, RewriteWords(expect, {{"at", place}, {"heading", Num(h)}}))});
}

Cmd PlaceCar(const std::string& car, const double at[3], double heading, bool set_heading) {
    Cmd c = Make("car.place");
    c.args.Set("id", car);
    c.args.Set("pos", Vec3Json(at));
    if (set_heading) c.args.Set("heading", std::round(heading * 10.0) / 10.0);
    return c;
}

Cmd SetCarHeading(const EditContext& ctx, const std::vector<dir::SnapEvent>& events, const std::string& car, double heading) {
    const int line = SpawnLineOf(events, car);
    const dir::SnapEvent* e = EventAt(events, line);
    if (!e) return Cmd{};
    return EditCmd(ctx.rev, {OpParam(line, ctx.ExpectFor(line, e->text), "heading", Num(std::fmod(std::fmod(heading, 360.0) + 360.0, 360.0)))});
}

Cmd SetCarPaint(const EditContext& ctx, const std::vector<dir::SnapEvent>& events, const std::string& car, int paint) {
    const int line = SpawnLineOf(events, car);
    const dir::SnapEvent* e = EventAt(events, line);
    if (!e) return Cmd{};
    return EditCmd(ctx.rev, {OpParam(line, ctx.ExpectFor(line, e->text), "paint", std::to_string(std::clamp(paint, 0, 9999)))});
}

Cmd DeleteCar(const EditContext& ctx, const std::vector<dir::SnapEvent>& events, const std::string& car) {
    std::vector<dir::EditOp> ops;
    for (const int line : LinesOfCar(events, car)) {
        const dir::SnapEvent* e = EventAt(events, line);
        if (e) ops.push_back(OpDelete(line, ctx.ExpectFor(line, e->text)));
    }
    if (ops.empty()) return Cmd{};
    return EditCmd(ctx.rev, ops);
}

std::vector<std::string> FilterModels(const std::vector<std::string>& all, const std::string& search, const std::vector<std::string>& recents, size_t max_count) {
    std::vector<std::string> words;
    {
        std::string cur;
        for (char c : search) {
            if (c == ' ' || c == '\t') {
                if (!cur.empty()) words.push_back(Lower(cur));
                cur.clear();
            } else {
                cur += c;
            }
        }
        if (!cur.empty()) words.push_back(Lower(cur));
    }
    const auto matches = [&](const std::string& name) {
        const std::string l = Lower(name);
        for (const std::string& w : words) {
            if (l.find(w) == std::string::npos) return false;
        }
        return true;
    };
    std::vector<std::string> out;
    std::set<std::string> seen;
    for (const std::string& r : recents) {
        if (matches(r) && std::find(all.begin(), all.end(), r) != all.end() && seen.insert(r).second) out.push_back(r);
    }
    for (const std::string& name : all) {
        if (out.size() >= max_count) break;
        if (matches(name) && seen.insert(name).second) out.push_back(name);
    }
    return out;
}

void NoteRecent(std::vector<std::string>* recents, const std::string& model, size_t keep) {
    recents->erase(std::remove(recents->begin(), recents->end(), model), recents->end());
    recents->insert(recents->begin(), model);
    if (recents->size() > keep) recents->resize(keep);
}

std::string ModelTitle(const std::string& model) {
    std::string s = model;
    if (s.rfind("vp_", 0) == 0) s = s.substr(3);
    for (char& c : s) {
        if (c == '_') c = ' ';
        else if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    return s;
}

// ---------------------------------------------------------------------------------------------------------------- paths

RouteDoc RouteFromReply(const std::string& name, const dir::Json& result) {
    RouteDoc r;
    r.name = name;
    for (const dir::Json& p : result.Get("points").items()) {
        if (!p.is_array() || p.size() < 3) continue;
        WayPt w;
        w.x = p.at(0).AsNumber();
        w.y = p.at(1).AsNumber();
        w.z = p.at(2).AsNumber();
        w.speed = p.size() > 3 ? p.at(3).AsNumber(14.0) : 14.0;
        r.pts.push_back(w);
    }
    r.loaded = true;
    return r;
}

std::vector<std::vector<double>> GizmoPoints(const RouteDoc& route) {
    std::vector<std::vector<double>> out;
    for (const WayPt& p : route.pts) out.push_back({p.x, p.y, p.z, p.speed});
    return out;
}

double RouteLengthM(const RouteDoc& route) {
    double len = 0.0;
    for (size_t i = 0; i + 1 < route.pts.size(); ++i) len += std::hypot(route.pts[i + 1].x - route.pts[i].x, route.pts[i + 1].z - route.pts[i].z);
    return len;
}

double RouteSeconds(const RouteDoc& route) {
    double t = 0.0;
    for (size_t i = 0; i + 1 < route.pts.size(); ++i) {
        const double d = std::hypot(route.pts[i + 1].x - route.pts[i].x, route.pts[i + 1].z - route.pts[i].z);
        t += d / std::max(0.5, 0.5 * (route.pts[i].speed + route.pts[i + 1].speed));
    }
    return t;
}

Cmd GetRoute(const std::string& name) {
    Cmd c = Make("path.get");
    c.args.Set("route", name);
    return c;
}

Cmd SetRoute(const RouteDoc& route) {
    Cmd c = Make("path.set");
    c.args.Set("route", route.name);
    dir::Json pts = dir::Json::Array();
    for (const WayPt& p : route.pts) {
        dir::Json q = dir::Json::Array();
        q.Push(std::round(p.x * 100.0) / 100.0);
        q.Push(std::round(p.y * 100.0) / 100.0);
        q.Push(std::round(p.z * 100.0) / 100.0);
        q.Push(std::round(p.speed * 10.0) / 10.0);
        pts.Push(std::move(q));
    }
    c.args.Set("points", std::move(pts));
    return c;
}

Cmd DropWaypoint(const std::string& route, const double at[3], double speed) {
    Cmd c = Make("path.waypoint");
    c.args.Set("route", route);
    c.args.Set("at", Vec3Json(at));
    c.args.Set("speed", speed);
    return c;
}

Cmd DropWaypointAtCamera(const std::string& route, double speed) {
    Cmd c = Make("path.waypoint");
    c.args.Set("route", route);
    c.args.Set("at", "camera");
    c.args.Set("speed", speed);
    return c;
}

Cmd StartRecording(const std::string& car, const std::string& name, double every_m) {
    Cmd c = Make("car.path.record");
    c.args.Set("id", car);
    c.args.Set("name", name);
    c.args.Set("every_m", every_m);
    return c;
}

Cmd StopRecording() {
    return Make("car.path.record.stop");
}

Cmd AssignRoute(const std::string& car, const std::string& route, double speed, double t, bool use_t) {
    Cmd c = Make("car.path");
    c.args.Set("id", car);
    c.args.Set("route", route);
    if (speed > 0.0) c.args.Set("speed", speed);
    if (use_t) c.args.Set("t", t);
    return c;
}

void RouteInsertAfter(RouteDoc* route, size_t index) {
    if (route->pts.empty() || index >= route->pts.size()) return;
    WayPt n;
    if (index + 1 < route->pts.size()) {
        const WayPt& a = route->pts[index];
        const WayPt& b = route->pts[index + 1];
        n = {(a.x + b.x) * 0.5, (a.y + b.y) * 0.5, (a.z + b.z) * 0.5, (a.speed + b.speed) * 0.5};
    } else {
        n = route->pts[index];
        double dx = 0.0, dz = 10.0;
        if (index > 0) {
            dx = n.x - route->pts[index - 1].x;
            dz = n.z - route->pts[index - 1].z;
            const double len = std::hypot(dx, dz);
            if (len > 1e-6) dx = dx / len * 10.0, dz = dz / len * 10.0;
            else dx = 0.0, dz = 10.0;
        }
        n.x += dx;
        n.z += dz;
    }
    route->pts.insert(route->pts.begin() + static_cast<std::ptrdiff_t>(index) + 1, n);
}

bool RouteDelete(RouteDoc* route, size_t index) {
    if (route->pts.size() <= 2 || index >= route->pts.size()) return false;
    route->pts.erase(route->pts.begin() + static_cast<std::ptrdiff_t>(index));
    return true;
}

void RouteMove(RouteDoc* route, size_t index, double x, double z) {
    if (index >= route->pts.size()) return;
    route->pts[index].x = x;
    route->pts[index].z = z;
}

void RouteSetSpeed(RouteDoc* route, size_t index, double speed) {
    if (index >= route->pts.size()) return;
    route->pts[index].speed = std::clamp(speed, 0.5, 120.0);
}

void RouteSetAllSpeeds(RouteDoc* route, double speed) {
    for (WayPt& p : route->pts) p.speed = std::clamp(speed, 0.5, 120.0);
}

RouteDoc SmoothRoute(const RouteDoc& route, int passes) {
    RouteDoc cur = route;
    for (int pass = 0; pass < passes && cur.pts.size() >= 3 && cur.pts.size() < 2000; ++pass) {
        RouteDoc next;
        next.name = cur.name;
        next.loaded = cur.loaded;
        next.pts.push_back(cur.pts.front());
        for (size_t i = 0; i + 1 < cur.pts.size(); ++i) {
            const WayPt& a = cur.pts[i];
            const WayPt& b = cur.pts[i + 1];
            const auto mix = [&](double u) {
                return WayPt{a.x + (b.x - a.x) * u, a.y + (b.y - a.y) * u, a.z + (b.z - a.z) * u, a.speed + (b.speed - a.speed) * u};
            };
            if (i > 0) next.pts.push_back(mix(0.25));
            if (i + 2 < cur.pts.size()) next.pts.push_back(mix(0.75));
        }
        next.pts.push_back(cur.pts.back());
        cur = std::move(next);
    }
    return cur;
}

double RouteStartHeading(const RouteDoc& route) {
    for (size_t i = 1; i < route.pts.size(); ++i) {
        const double dx = route.pts[i].x - route.pts[0].x, dz = route.pts[i].z - route.pts[0].z;
        if (std::hypot(dx, dz) > 0.5) return std::atan2(dx, dz) * 180.0 / kPi;
    }
    return 0.0;
}

std::vector<Cmd> ReplayCmds(const std::string& car_id, const std::string& model, const RouteDoc& route, double path_t, double speed) {
    std::vector<Cmd> out;
    if (route.pts.empty()) return out;
    const double at[3] = {route.pts[0].x, route.pts[0].y, route.pts[0].z};
    out.push_back(SpawnCar(car_id, model, at, RouteStartHeading(route), 0.0, -1));
    out.push_back(AssignRoute(car_id, route.name, speed, path_t, true));
    return out;
}

std::string SafeRouteName(const std::string& text) {
    std::string out;
    for (char c : text) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
        if (ok) out += c;
        else if (c == ' ') out += '-';
        if (out.size() >= 40) break;
    }
    return out;
}

// ---------------------------------------------------------------------------------------------------------------- take

Cmd TakeRenderCmd(const TakeForm& form) {
    Cmd c = Make("take.render");
    c.args.Set("fps", form.fps);
    c.args.Set("sink", kSinks[std::clamp(form.sink, 0, 3)]);
    if (!form.name.empty()) c.args.Set("name", SafeRouteName(form.name));
    if (form.from > 0.0) c.args.Set("from", form.from);
    if (form.to >= 0.0) c.args.Set("to", form.to);
    return c;
}

Cmd TakeStopHint() {
    return Make("take.status");
}

// ---------------------------------------------------------------------------------------------------------------- flying

fc::Pad MakeFlyPad(const FlyKeys& k, const FlyTuning& tuning, double fov_degrees) {
    fc::Pad pad;
    pad.ly = (k.forward ? 1.0 : 0.0) - (k.back ? 1.0 : 0.0);
    pad.lx = (k.right ? 1.0 : 0.0) - (k.left ? 1.0 : 0.0);
    pad.rt = k.up ? 1.0 : 0.0;
    pad.lt = k.down ? 1.0 : 0.0;
    if (k.fast) pad.buttons |= fc::kBtnL3;
    if (k.slow) pad.buttons |= fc::kBtnR3;
    if (k.roll_left) pad.buttons |= fc::kBtnLb;
    if (k.roll_right) pad.buttons |= fc::kBtnRb;
    if (k.level) pad.buttons |= fc::kBtnY;
    if (k.zoom_in) pad.buttons |= fc::kBtnDpadUp;
    if (k.zoom_out) pad.buttons |= fc::kBtnDpadDown;
    if (k.look) {
        // a narrower view turns more finely: the same pixel is a smaller angle when zoomed in
        const double scale = std::clamp(fov_degrees, 10.0, 110.0) / 50.0;
        pad.look_right = static_cast<double>(k.mouse_dx) * tuning.degrees_per_pixel * scale;
        pad.look_up = -static_cast<double>(k.mouse_dy) * tuning.degrees_per_pixel * scale;
    }
    pad.speed_steps = std::clamp(static_cast<int>(std::lround(static_cast<double>(k.wheel) * tuning.speed_per_notch)), -10, 10);
    return pad;
}

bool AnyFlyInput(const FlyKeys& k) {
    return k.forward || k.back || k.left || k.right || k.down || k.up || k.roll_left || k.roll_right || k.level || k.zoom_in || k.zoom_out || (k.look && (k.mouse_dx != 0.0f || k.mouse_dy != 0.0f)) ||
           k.wheel != 0.0f;
}

std::vector<Cmd> CommandsForFlyEvents(const fc::Events& ev, const dir::Snapshot* snap, const TakeForm& take) {
    std::vector<Cmd> out;
    if (ev.add_key) out.push_back(AddKeyHere(0.0, false, "", false));
    if (snap) {
        if (ev.next_key) {
            for (Cmd& c : GoToKeyCmds(*snap, +1)) out.push_back(std::move(c));
        }
        if (ev.prev_key) {
            for (Cmd& c : GoToKeyCmds(*snap, -1)) out.push_back(std::move(c));
        }
        if (ev.play_pause) out.push_back(TogglePlayCmd(snap->transport));
    } else if (ev.play_pause) {
        out.push_back(PlayCmd());
    }
    if (ev.step_frame) {
        if (snap && snap->transport.mode != "fixed") out.push_back(FixedModeCmd(true));  // a step needs the fixed step
        out.push_back(StepCmd(1));
    }
    if (ev.render_take) out.push_back(TakeRenderCmd(take));
    return out;
}

// ---------------------------------------------------------------------------------------------------------------- the transport

Cmd PlayCmd() {
    return Make("transport.play");
}
Cmd PauseCmd() {
    return Make("transport.pause");
}
Cmd RestartCmd(double t) {
    Cmd c = Make("transport.restart");
    if (t > 0.0) c.args.Set("t", t);
    return c;
}
Cmd SeekCmd(double t) {
    Cmd c = Make("transport.seek");
    c.args.Set("t", std::max(0.0, t));
    return c;
}
Cmd StepCmd(int frames) {
    Cmd c = Make("transport.step");
    c.args.Set("frames", std::max(1, frames));
    return c;
}
Cmd FixedModeCmd(bool fixed) {
    Cmd c = Make("transport.mode");
    c.args.Set("fixed", fixed);
    return c;
}
Cmd CameraModeCmd(const std::string& mode) {
    Cmd c = Make("camera.mode");
    c.args.Set("mode", mode);
    return c;
}
Cmd CameraSetCmd(const dir::SnapPose& pose) {
    Cmd c = Make("camera.set");
    c.args.Set("pos", Vec3Json(pose.pos));
    c.args.Set("yaw", std::round(pose.yaw * 100.0) / 100.0);
    c.args.Set("pitch", std::round(pose.pitch * 100.0) / 100.0);
    c.args.Set("roll", std::round(pose.roll * 100.0) / 100.0);
    c.args.Set("fov", std::round(pose.fov * 100.0) / 100.0);
    return c;
}
Cmd LookAtCmd(const std::string& target) {
    Cmd c = Make("camera.look_at");
    c.args.Set("target", target);
    return c;
}
Cmd TogglePlayCmd(const dir::SnapTransport& t) {
    return t.paused ? PlayCmd() : PauseCmd();
}

// ---------------------------------------------------------------------------------------------------------------- small things

double FrameStep(const dir::SnapTransport& t) {
    if (t.step > 1e-6) return t.step;
    if (t.fps > 1.0) return 1.0 / t.fps;
    return 1.0 / 60.0;
}

std::string TransportStateText(const dir::SnapTransport& t) {
    if (t.holding_for_ready) return "WAITING FOR THE CARS";
    if (t.paused && t.frozen) return "PAUSED (WORLD FROZEN)";
    if (t.paused) return "PAUSED";
    std::string s = t.state;
    for (char& c : s) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    return s;
}

std::string CameraModeText(const std::string& mode) {
    if (mode == "freecam") return "FREE CAMERA";
    if (mode == "scene") return "SCENE CAMERA";
    return "GAME CAMERA";
}

std::vector<std::string> CarIds(const dir::Snapshot& snap) {
    std::vector<std::string> ids;
    ids.push_back("player");
    for (const dir::SnapCar& c : snap.cars) {
        if (std::find(ids.begin(), ids.end(), c.id) == ids.end()) ids.push_back(c.id);
    }
    return ids;
}

}  // namespace mcla::ui::director
