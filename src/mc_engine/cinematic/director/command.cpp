// The Director's engine room: see command.h.

#include "command.h"
#include "settings/immortal.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <exception>
#include <utility>

namespace mc::cinematic::director {

// ---------------------------------------------------------------------------------------------------------------- names

const char* SourceName(Source source) {
    switch (source) {
        case Source::kAgent: return "agent";
        case Source::kPanel: return "panel";
        case Source::kConsole: return "console";
        case Source::kInternal: return "internal";
    }
    return "?";
}

const char* ArgTypeName(ArgType type) {
    switch (type) {
        case ArgType::kAny: return "any";
        case ArgType::kBool: return "bool";
        case ArgType::kNumber: return "number";
        case ArgType::kInt: return "int";
        case ArgType::kString: return "string";
        case ArgType::kArray: return "array";
        case ArgType::kObject: return "object";
        case ArgType::kVec3: return "vec3";
        case ArgType::kTarget: return "target";
    }
    return "any";
}

const char* EventKindName(EventKind kind) {
    switch (kind) {
        case EventKind::kCamera: return "camera";
        case EventKind::kCut: return "cut";
        case EventKind::kCar: return "car";
        case EventKind::kWorld: return "world";
        case EventKind::kHud: return "hud";
        case EventKind::kCapture: return "capture";
        case EventKind::kSplit: return "split";
        case EventKind::kLabel: return "label";
        case EventKind::kMark: return "mark";
        case EventKind::kSetting: return "setting";
        case EventKind::kOther: return "other";
    }
    return "other";
}

const std::vector<EventDoc>& KnownEvents() {
    static const std::vector<EventDoc>& events = *new const std::vector<EventDoc>{
        {"scene.reloaded", "the scene file changed: rev, by (agent, panel, file, undo, accept), changed (new line numbers), errors"},
        {"scene.state", "the scene runner's state changed: state, t, path"},
        {"key.reached", "the scene's clock passed a key: t, line, track, text"},
        {"car.ready", "a scene car is made and readable: id"},
        {"shot.done", "a still was written: path, t, ok, error? (index, total for a schedule)"},
        {"take.progress", "a take is running: name, frames"},
        {"take.done", "a take ended: name, folder, state, frames, error?"},
        {"edit", "a client changed the scene: rev, by, ops, lines"},
        {"agent", "an agent connected or left: client, name, connected"},
    };
    return events;
}

bool IsQuietCommand(std::string_view name) {
    if (name == "ping" || name == "help" || name == "log.tail" || name == "events.set" || name == "take.status") return true;
    const auto ends_with = [&](std::string_view suffix) { return name.size() >= suffix.size() && name.substr(name.size() - suffix.size()) == suffix; };
    return ends_with(".get") || ends_with(".list");
}

// ---------------------------------------------------------------------------------------------------------------- replies

Json Reply::ToJson() const {
    Json o = Json::Object();
    if (has_id) o.Set("id", id);
    o.Set("ok", ok);
    if (ok) {
        o.Set("result", result.is_object() ? result : Json::Object());
    } else {
        Json e = Json::Object();
        e.Set("code", ErrorCodeName(code));
        e.Set("msg", message);
        if (details.is_object()) {
            for (const Json::Member& m : details.members()) {
                if (m.first != "code" && m.first != "msg") e.Set(m.first, m.second);
            }
        }
        o.Set("error", std::move(e));
    }
    return o;
}

void Responder::Done(Json result) const {
    if (!slot_ || slot_->finished) return;
    slot_->reply = Reply{};
    slot_->reply.ok = true;
    if (result.is_object()) {
        slot_->reply.result = std::move(result);
    } else if (result.is_null()) {
        slot_->reply.result = Json::Object();
    } else {
        Json wrapped = Json::Object();
        wrapped.Set("value", std::move(result));
        slot_->reply.result = std::move(wrapped);
    }
    if (slot_->engine) slot_->engine->Finish(*slot_);
}

void Responder::Fail(ErrorCode code, std::string_view message, Json details) const {
    if (!slot_ || slot_->finished) return;
    slot_->reply = Reply{};
    slot_->reply.ok = false;
    slot_->reply.code = code;
    slot_->reply.message = std::string(message);
    slot_->reply.details = std::move(details);
    if (slot_->engine) slot_->engine->Finish(*slot_);
}

void Responder::Defer(std::function<void(const Responder&)> poll) const {
    if (!slot_ || slot_->finished) return;
    slot_->deferred = true;
    slot_->poll = std::move(poll);
}

bool Responder::finished() const {
    return slot_ && slot_->finished;
}

bool Responder::deferred() const {
    return slot_ && slot_->deferred && !slot_->finished;
}

const Request& Responder::request() const {
    static const Request& none = larecomp::Leak<const Request>();
    return slot_ ? slot_->request : none;
}

// ---------------------------------------------------------------------------------------------------------------- argument check

namespace {

const char* TypePhrase(ArgType t) {
    switch (t) {
        case ArgType::kAny: return "anything";
        case ArgType::kBool: return "true or false";
        case ArgType::kNumber: return "a number";
        case ArgType::kInt: return "a whole number";
        case ArgType::kString: return "a string";
        case ArgType::kArray: return "an array";
        case ArgType::kObject: return "an object";
        case ArgType::kVec3: return "[x, y, z] (three numbers)";
        case ArgType::kTarget: return "a target: \"car:p1\", \"player\" or [x, y, z]";
    }
    return "valid";
}

bool TypeFits(ArgType t, const Json& v) {
    switch (t) {
        case ArgType::kAny: return true;
        case ArgType::kBool: return v.is_bool();
        case ArgType::kNumber: return v.is_number() && std::isfinite(v.AsNumber());
        case ArgType::kInt: return v.is_integer();
        case ArgType::kString: return v.is_string();
        case ArgType::kArray: return v.is_array();
        case ArgType::kObject: return v.is_object();
        case ArgType::kVec3: {
            double d[3];
            return v.ToVec3(d);
        }
        case ArgType::kTarget: {
            double d[3];
            return v.is_string() || v.ToVec3(d);
        }
    }
    return false;
}

}  // namespace

std::string ValidateArgs(const Schema& schema, const Json& args, std::string_view cmd) {
    const std::string prefix = cmd.empty() ? std::string() : std::string(cmd) + ": ";
    if (!args.is_object()) return prefix + "'args' must be an object";
    for (const ArgSpec& a : schema.args) {
        const Json* v = args.Find(a.name);
        if (!v || v->is_null()) {
            if (a.required) return prefix + "'" + a.name + "' is required";
            continue;
        }
        if (!TypeFits(a.type, *v)) return prefix + "'" + a.name + "' must be " + TypePhrase(a.type);
    }
    if (!schema.open_args) {
        for (const Json::Member& m : args.members()) {
            const bool known = std::any_of(schema.args.begin(), schema.args.end(), [&](const ArgSpec& a) { return a.name == m.first; });
            if (!known) return prefix + "unknown argument '" + m.first + "'";
        }
    }
    return {};
}

// ---------------------------------------------------------------------------------------------------------------- the engine

namespace {

double SteadySeconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

struct BatchState {
    std::vector<Json> cmds;
    bool stop_on_error = true;
    Json results = Json::Array();
    size_t next = 0;
    std::shared_ptr<detail::ReplySlot> child;
    Responder outer;
    Source source = Source::kInternal;
    int client = -1;
    bool done = false;
};

void BatchStep(Engine& engine, const std::shared_ptr<BatchState>& st) {
    if (st->done) return;
    const auto collect = [&](const detail::ReplySlot& c) {
        Reply r = c.reply;
        r.has_id = false;
        st->results.Push(r.ToJson());
        return r.ok;
    };
    const auto finish = [&]() {
        st->done = true;
        Json res = Json::Object();
        res.Set("results", std::move(st->results));
        st->outer.Done(std::move(res));
    };
    if (st->child) {
        if (!st->child->finished) return;
        const bool ok = collect(*st->child);
        st->child.reset();
        if (!ok && st->stop_on_error) return finish();
    }
    while (st->next < st->cmds.size()) {
        const Json& sub = st->cmds[st->next++];
        Request req;
        req.cmd = sub.StringOr("cmd", "");
        req.args = sub.Get("args").is_object() ? sub.Get("args") : Json::Object();
        req.source = st->source;
        req.client = st->client;
        req.wait_settled = sub.StringOr("wait", "") == "settled";
        const double to = sub.NumberOr("timeout_s", 0.0);
        req.timeout_s = to > 0.0 ? std::min(to, 3600.0) : kDefaultTimeoutS;
        auto child = engine.RunNow(std::move(req), nullptr);
        if (!child->finished) {
            st->child = child;
            if (!st->outer.deferred()) st->outer.Defer([&engine, st](const Responder&) { BatchStep(engine, st); });
            return;
        }
        if (!collect(*child) && st->stop_on_error) return finish();
    }
    finish();
}

}  // namespace

Engine::Engine(NowFn now) : now_(now ? std::move(now) : NowFn(&SteadySeconds)) {
    RegisterBuiltins();
}

bool Engine::Register(std::string_view name, Handler handler, Schema schema) {
    if (name.empty() || !handler) return false;
    std::lock_guard<std::mutex> lock(registry_mutex_);
    if (commands_.find(name) != commands_.end()) return false;
    commands_.emplace(std::string(name), Entry{std::move(handler), std::move(schema)});
    return true;
}

bool Engine::Has(std::string_view name) const {
    std::lock_guard<std::mutex> lock(registry_mutex_);
    return commands_.find(name) != commands_.end();
}

bool Engine::SchemaOf(std::string_view name, Schema* out) const {
    std::lock_guard<std::mutex> lock(registry_mutex_);
    const auto it = commands_.find(name);
    if (it == commands_.end()) return false;
    if (out) *out = it->second.schema;
    return true;
}

std::vector<std::string> Engine::Names() const {
    std::lock_guard<std::mutex> lock(registry_mutex_);
    std::vector<std::string> names;
    names.reserve(commands_.size());
    for (const auto& kv : commands_) names.push_back(kv.first);
    return names;
}

Json Engine::HelpJson(std::string_view cmd) const {
    Json out = Json::Object();
    out.Set("proto", kProtocolVersion);
    Json list = Json::Array();
    {
        std::lock_guard<std::mutex> lock(registry_mutex_);
        for (const auto& kv : commands_) {
            if (!cmd.empty() && kv.first != cmd) continue;
            const Schema& s = kv.second.schema;
            Json c = Json::Object();
            c.Set("name", kv.first);
            c.Set("group", s.group);
            c.Set("summary", s.summary);
            c.Set("file", s.file);
            c.Set("open_args", s.open_args);
            Json args = Json::Array();
            for (const ArgSpec& a : s.args) {
                Json j = Json::Object();
                j.Set("name", a.name);
                j.Set("type", ArgTypeName(a.type));
                j.Set("required", a.required);
                j.Set("doc", a.doc);
                args.Push(std::move(j));
            }
            c.Set("args", std::move(args));
            c.Set("result", s.result);
            list.Push(std::move(c));
        }
    }
    out.Set("commands", std::move(list));
    Json errors = Json::Array();
    for (int i = 0; i <= static_cast<int>(ErrorCode::kFailed); ++i) errors.Push(ErrorCodeName(static_cast<ErrorCode>(i)));
    out.Set("errors", std::move(errors));
    Json events = Json::Array();
    for (const EventDoc& e : KnownEvents()) {
        Json j = Json::Object();
        j.Set("name", e.name);
        j.Set("doc", e.doc);
        events.Push(std::move(j));
    }
    out.Set("events", std::move(events));
    return out;
}

void Engine::RegisterBuiltins() {
    {
        Schema s;
        s.group = "core";
        s.summary = "every command with its arguments, the error codes and the events";
        s.args = {{"cmd", ArgType::kString, false, "describe only this command"}};
        s.result = "{proto, commands:[{name, group, summary, file, open_args, args:[{name,type,required,doc}], result}], errors, events}";
        Register(
            "help",
            [this](const Request& req, const Responder& r) {
                const std::string name = req.args.StringOr("cmd", "");
                if (!name.empty() && !Has(name)) return r.Fail(ErrorCode::kUnknownCmd, "no such command '" + name + "' (help lists them)");
                r.Done(HelpJson(name));
            },
            std::move(s));
    }
    {
        Schema s;
        s.group = "core";
        s.summary = "is the game there";
        s.result = "{pong, frame, t, state}";
        Register(
            "ping",
            [this](const Request&, const Responder& r) {
                Json res = Json::Object();
                res.Set("pong", true);
                if (ping_fields_) ping_fields_(res);
                r.Done(std::move(res));
            },
            std::move(s));
    }
    {
        Schema s;
        s.group = "core";
        s.summary = "several commands in one update, in order";
        s.args = {{"cmds", ArgType::kArray, true, "[{cmd, args?, wait?, timeout_s?}]"}, {"stop_on_error", ArgType::kBool, false, "default true"}};
        s.result = "{results:[{ok, result|error}]}";
        Register(
            "batch",
            [this](const Request& req, const Responder& r) {
                const Json& cmds = req.args.Get("cmds");
                if (cmds.size() == 0) return r.Fail(ErrorCode::kBadArgs, "batch: 'cmds' is empty");
                if (cmds.size() > 256) return r.Fail(ErrorCode::kBadArgs, "batch: at most 256 commands");
                auto st = std::make_shared<BatchState>();
                for (size_t i = 0; i < cmds.size(); ++i) {
                    const Json& c = cmds.at(i);
                    if (!c.is_object() || !c.Get("cmd").is_string() || c.Get("cmd").AsString().empty()) {
                        return r.Fail(ErrorCode::kBadArgs, "batch: cmds[" + std::to_string(i) + "] must be {cmd, args?}");
                    }
                    if (c.Get("cmd").AsString() == "batch") return r.Fail(ErrorCode::kBadArgs, "batch: a batch cannot hold a batch");
                    if (c.Find("args") && !c.Get("args").is_null() && !c.Get("args").is_object()) {
                        return r.Fail(ErrorCode::kBadArgs, "batch: cmds[" + std::to_string(i) + "].args must be an object");
                    }
                    st->cmds.push_back(c);
                }
                st->stop_on_error = req.args.BoolOr("stop_on_error", true);
                st->outer = r;
                st->source = req.source;
                st->client = req.client;
                BatchStep(*this, st);
            },
            std::move(s));
    }
    {
        Schema s;
        s.group = "core";
        s.summary = "mute this connection's events, or take only the named ones";
        s.args = {{"on", ArgType::kBool, false, "false mutes every event"}, {"only", ArgType::kArray, false, "event names; [] = all"}};
        s.result = "{on, only}";
        Register(
            "events.set",
            [this](const Request& req, const Responder& r) {
                Json res = Json::Object();
                const int c = req.client;
                if (c >= 0 && c < static_cast<int>(kMaxClients)) {
                    std::lock_guard<std::mutex> lock(events_mutex_);
                    ClientFilter& f = filters_[static_cast<size_t>(c)];
                    if (req.args.Find("on")) f.on = req.args.BoolOr("on", true);
                    if (const Json* only = req.args.Find("only"); only && only->is_array()) {
                        f.only.clear();
                        for (const Json& n : only->items()) {
                            if (n.is_string()) f.only.push_back(n.AsString());
                        }
                        f.all = f.only.empty();
                    }
                    res.Set("on", f.on);
                    Json names = Json::Array();
                    if (!f.all) {
                        for (const std::string& n : f.only) names.Push(n);
                        res.Set("only", std::move(names));
                    } else {
                        res.Set("only", Json());
                    }
                } else {
                    res.Set("on", true);
                    res.Set("only", Json());
                }
                r.Done(std::move(res));
            },
            std::move(s));
    }
}

void Engine::SetPingFields(std::function<void(Json&)> fn) {
    ping_fields_ = std::move(fn);
}

// ---- intake

void Engine::SetEnabled(bool on) {
    const bool was = enabled_.exchange(on, std::memory_order_acq_rel);
    if (on || !was) return;
    std::deque<Queued> dropped;
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        dropped.swap(queue_);
    }
    for (Queued& q : dropped) {
        if (!q.posted.on_reply) continue;
        Reply r;
        r.id = q.posted.id;
        r.has_id = q.posted.has_id;
        r.ok = false;
        r.code = ErrorCode::kRefused;
        r.message = "the Director is off";
        q.posted.on_reply(r);
    }
}

bool Engine::Post(Posted command) {
    if (!enabled_.load(std::memory_order_acquire)) return false;
    std::lock_guard<std::mutex> lock(queue_mutex_);
    if (queue_.size() >= kMaxQueued) return false;
    Queued q;
    q.posted = std::move(command);
    q.seq = next_seq_.fetch_add(1, std::memory_order_relaxed);
    queue_.push_back(std::move(q));
    return true;
}

size_t Engine::queued() const {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    return queue_.size();
}

// ---- running

std::shared_ptr<detail::ReplySlot> Engine::RunNow(Request request, std::function<void(const Reply&)> on_reply) {
    auto slot = std::make_shared<detail::ReplySlot>();
    if (request.seq == 0) request.seq = next_seq_.fetch_add(1, std::memory_order_relaxed);
    slot->request = std::move(request);
    slot->on_reply = std::move(on_reply);
    slot->engine = this;
    slot->deadline = Now() + (slot->request.timeout_s > 0.0 ? slot->request.timeout_s : kDefaultTimeoutS);
    Dispatch(slot);
    return slot;
}

void Engine::Dispatch(const std::shared_ptr<detail::ReplySlot>& slot) {
    const Request& req = slot->request;
    const Responder r(slot);
    Handler handler;
    Schema schema;
    {
        std::lock_guard<std::mutex> lock(registry_mutex_);
        const auto it = commands_.find(req.cmd);
        if (it != commands_.end()) {
            handler = it->second.handler;
            schema = it->second.schema;
        }
    }
    if (!handler) {
        r.Fail(ErrorCode::kUnknownCmd, "no such command '" + req.cmd + "' (help lists them)");
        return;
    }
    if (const std::string bad = ValidateArgs(schema, req.args, req.cmd); !bad.empty()) {
        r.Fail(ErrorCode::kBadArgs, bad);
        return;
    }
    try {
        handler(req, r);
    } catch (const std::exception& e) {
        r.Fail(ErrorCode::kFailed, std::string(req.cmd) + ": the handler threw: " + e.what());
    } catch (...) {
        r.Fail(ErrorCode::kFailed, std::string(req.cmd) + ": the handler threw");
    }
    if (slot->finished) return;
    if (!slot->deferred) {
        r.Fail(ErrorCode::kFailed, req.cmd + ": the command made no reply");
        return;
    }
    if (!slot->queued_for_poll) {
        slot->queued_for_poll = true;
        deferred_.push_back(slot);
    }
}

Engine::RunStats Engine::RunQueued(int max_commands, double budget_ms) {
    RunStats stats;
    const double start = Now();
    while (stats.ran < max_commands) {
        Queued q;
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            if (queue_.empty()) break;
            q = std::move(queue_.front());
            queue_.pop_front();
        }
        Request req;
        req.seq = q.seq;
        req.id = q.posted.id;
        req.has_id = q.posted.has_id;
        req.cmd = std::move(q.posted.cmd);
        req.args = q.posted.args.is_object() ? std::move(q.posted.args) : Json::Object();
        req.source = q.posted.source;
        req.client = q.posted.client;
        req.wait_settled = q.posted.wait_settled;
        req.timeout_s = q.posted.timeout_s > 0.0 ? q.posted.timeout_s : kDefaultTimeoutS;
        RunNow(std::move(req), std::move(q.posted.on_reply));
        ++stats.ran;
        if ((Now() - start) * 1000.0 >= budget_ms) break;
    }
    stats.left = queued();
    return stats;
}

void Engine::PollDeferred() {
    if (deferred_.empty()) return;
    std::vector<std::shared_ptr<detail::ReplySlot>> list;
    list.swap(deferred_);
    const double now = Now();
    for (const auto& slot : list) {
        if (slot->finished) {
            slot->queued_for_poll = false;
            continue;
        }
        const Responder r(slot);
        if (now >= slot->deadline) {
            char buf[96];
            std::snprintf(buf, sizeof(buf), "%s: no answer in %.1f s", slot->request.cmd.c_str(), slot->request.timeout_s);
            r.Fail(ErrorCode::kTimeout, buf);
        } else if (slot->poll) {
            try {
                slot->poll(r);
            } catch (const std::exception& e) {
                r.Fail(ErrorCode::kFailed, slot->request.cmd + ": the poll threw: " + e.what());
            } catch (...) {
                r.Fail(ErrorCode::kFailed, slot->request.cmd + ": the poll threw");
            }
        } else {
            r.Fail(ErrorCode::kFailed, slot->request.cmd + ": deferred with nothing to poll");
        }
        if (slot->finished) slot->queued_for_poll = false;
        else deferred_.push_back(slot);
    }
}

void Engine::Finish(detail::ReplySlot& slot) {
    if (slot.finished) return;
    slot.finished = true;
    slot.poll = nullptr;
    slot.reply.id = slot.request.id;
    slot.reply.has_id = slot.request.has_id;
    LogCommand(slot);
    if (slot.on_reply) {
        auto cb = std::move(slot.on_reply);
        slot.on_reply = nullptr;
        cb(slot.reply);
    }
}

void Engine::LogCommand(const detail::ReplySlot& slot) {
    if (slot.reply.ok && IsQuietCommand(slot.request.cmd)) return;
    LoggedCommand c;
    c.seq = slot.request.seq;
    c.source = slot.request.source;
    c.client = slot.request.client;
    c.line = FormatConsoleLine(slot.request.cmd, slot.request.args);
    if (c.line.size() > 300) c.line = c.line.substr(0, 297) + "...";
    c.ok = slot.reply.ok;
    if (!c.ok) c.error = std::string(ErrorCodeName(slot.reply.code)) + ": " + slot.reply.message;
    std::lock_guard<std::mutex> lock(log_mutex_);
    log_.push_back(std::move(c));
    while (log_.size() > 64) log_.pop_front();
}

std::vector<LoggedCommand> Engine::Recent(size_t max_count) const {
    std::lock_guard<std::mutex> lock(log_mutex_);
    const size_t n = std::min(max_count, log_.size());
    return std::vector<LoggedCommand>(log_.end() - static_cast<std::ptrdiff_t>(n), log_.end());
}

// ---- events

void Engine::Publish(std::string_view name, Json fields) {
    const std::string line = EventLine(name, fields);
    EventSink sink;
    std::vector<EventListener> listeners;
    {
        std::lock_guard<std::mutex> lock(events_mutex_);
        sink = sink_;
        for (const auto& kv : listeners_) listeners.push_back(kv.second);
    }
    if (sink) sink(line, name);
    if (!listeners.empty()) {
        Json ev;
        std::string err;
        if (Json::Parse(line, &ev, &err)) {
            for (const EventListener& l : listeners) l(name, ev);
        }
    }
}

int Engine::AddListener(EventListener listener) {
    std::lock_guard<std::mutex> lock(events_mutex_);
    const int token = next_listener_++;
    listeners_[token] = std::move(listener);
    return token;
}

void Engine::RemoveListener(int token) {
    std::lock_guard<std::mutex> lock(events_mutex_);
    listeners_.erase(token);
}

void Engine::SetEventSink(EventSink sink) {
    std::lock_guard<std::mutex> lock(events_mutex_);
    sink_ = std::move(sink);
}

bool Engine::ShouldSend(int client, std::string_view event_name) const {
    if (client < 0 || client >= static_cast<int>(kMaxClients)) return true;
    std::lock_guard<std::mutex> lock(events_mutex_);
    const ClientFilter& f = filters_[static_cast<size_t>(client)];
    if (!f.on) return false;
    if (f.all) return true;
    return std::find(f.only.begin(), f.only.end(), event_name) != f.only.end();
}

void Engine::ResetClientFilter(int client) {
    if (client < 0 || client >= static_cast<int>(kMaxClients)) return;
    std::lock_guard<std::mutex> lock(events_mutex_);
    filters_[static_cast<size_t>(client)] = ClientFilter{};
}

// ---- snapshot

void Engine::SetSnapshot(std::shared_ptr<const Snapshot> snapshot) {
    std::lock_guard<std::mutex> lock(snapshot_mutex_);
    snapshot_ = std::move(snapshot);
}

std::shared_ptr<const Snapshot> Engine::GetSnapshot() const {
    std::lock_guard<std::mutex> lock(snapshot_mutex_);
    return snapshot_;
}

// ---------------------------------------------------------------------------------------------------------------- the process's engine and the public free functions

Engine& GlobalEngine() {
    static Engine* engine = new Engine();  // never destroyed: handlers registered at static initialisation outlive main
    return *engine;
}

bool RegisterCommand(std::string_view name, Handler handler, Schema schema) {
    return GlobalEngine().Register(name, std::move(handler), std::move(schema));
}

bool Post(Posted command) {
    return GlobalEngine().Post(std::move(command));
}

void PublishEvent(std::string_view name, Json fields) {
    GlobalEngine().Publish(name, std::move(fields));
}

int AddEventListener(EventListener listener) {
    return GlobalEngine().AddListener(std::move(listener));
}

void RemoveEventListener(int token) {
    GlobalEngine().RemoveListener(token);
}

std::shared_ptr<const Snapshot> GetSnapshot() {
    return GlobalEngine().GetSnapshot();
}

std::vector<LoggedCommand> RecentCommands(size_t max_count) {
    return GlobalEngine().Recent(max_count);
}

namespace {
std::vector<SnapshotContributor>& Contributors() {
    static std::vector<SnapshotContributor>& v = larecomp::Leak<std::vector<SnapshotContributor>>();
    return v;
}
std::mutex& ContributorsMutex() {
    static std::mutex m;
    return m;
}
}  // namespace

void AddSnapshotContributor(SnapshotContributor contributor) {
    if (!contributor) return;
    std::lock_guard<std::mutex> lock(ContributorsMutex());
    Contributors().push_back(contributor);
}

std::vector<SnapshotContributor> SnapshotContributors() {
    std::lock_guard<std::mutex> lock(ContributorsMutex());
    return Contributors();
}

// ---------------------------------------------------------------------------------------------------------------- the wire

bool ParseRequest(const Json& object, Posted* out, std::string* error, int64_t* id_out, bool* has_id_out) {
    *has_id_out = false;
    *id_out = 0;
    if (!object.is_object()) {
        *error = "a request is a JSON object: {\"id\":1,\"cmd\":\"...\",\"args\":{...}}";
        return false;
    }
    if (const Json* id = object.Find("id"); id && !id->is_null()) {
        if (!id->is_integer()) {
            *error = "'id' must be an integer";
            return false;
        }
        *id_out = id->AsInt();
        *has_id_out = true;
    }
    Posted p;
    p.id = *id_out;
    p.has_id = *has_id_out;
    p.source = Source::kAgent;
    const Json& cmd = object.Get("cmd");
    if (!cmd.is_string() || cmd.AsString().empty()) {
        *error = "'cmd' must be a command name (a string)";
        return false;
    }
    p.cmd = cmd.AsString();
    const Json* args = object.Find("args");
    if (args && !args->is_null()) {
        if (!args->is_object()) {
            *error = "'args' must be an object";
            return false;
        }
        p.args = *args;
    } else {
        p.args = Json::Object();
    }
    if (const Json* wait = object.Find("wait"); wait && !wait->is_null()) {
        if (!wait->is_string() || wait->AsString() != "settled") {
            *error = "'wait' must be \"settled\"";
            return false;
        }
        p.wait_settled = true;
    }
    if (const Json* to = object.Find("timeout_s"); to && !to->is_null()) {
        if (!to->is_number() || !(to->AsNumber() > 0.0) || to->AsNumber() > 3600.0) {
            *error = "'timeout_s' must be a number of seconds, more than 0 and at most 3600";
            return false;
        }
        p.timeout_s = to->AsNumber();
    }
    *out = std::move(p);
    return true;
}

std::string ReplyLine(const Reply& reply) {
    return reply.ToJson().Dump();
}

std::string EventLine(std::string_view name, const Json& fields) {
    Json e = Json::Object();
    e.Set("event", name);
    if (fields.is_object()) {
        for (const Json::Member& m : fields.members()) {
            if (m.first != "event") e.Set(m.first, m.second);
        }
    }
    return e.Dump();
}

// ---------------------------------------------------------------------------------------------------------------- the console grammar

namespace {

bool IsIdentStart(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
bool IsIdentChar(char c) {
    return IsIdentStart(c) || (c >= '0' && c <= '9');
}

bool SplitTokens(std::string_view line, std::vector<std::string>* out, std::string* error) {
    size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
        if (i >= line.size()) break;
        std::string tok;
        bool in_quote = false;
        int depth = 0;
        for (; i < line.size(); ++i) {
            const char c = line[i];
            if (in_quote) {
                tok.push_back(c);
                if (c == '\\' && i + 1 < line.size()) {
                    tok.push_back(line[++i]);
                } else if (c == '"') {
                    in_quote = false;
                }
                continue;
            }
            if (c == '"') {
                in_quote = true;
            } else if (c == '[' || c == '{') {
                ++depth;
            } else if (c == ']' || c == '}') {
                --depth;
            } else if ((c == ' ' || c == '\t') && depth <= 0) {
                break;
            }
            tok.push_back(c);
        }
        if (in_quote) {
            *error = "a quote is not closed";
            return false;
        }
        if (depth != 0) {
            *error = "a bracket is not closed";
            return false;
        }
        out->push_back(std::move(tok));
    }
    return true;
}

bool LooksLikeNumber(std::string_view s) {
    size_t i = 0;
    if (i < s.size() && s[i] == '-') ++i;
    size_t digits = 0;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i, ++digits;
    if (digits == 0) return false;
    if (i < s.size() && s[i] == '.') {
        ++i;
        size_t frac = 0;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i, ++frac;
        if (frac == 0) return false;
    }
    if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
        ++i;
        if (i < s.size() && (s[i] == '+' || s[i] == '-')) ++i;
        size_t exp = 0;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i, ++exp;
        if (exp == 0) return false;
    }
    return i == s.size();
}

bool ParseNumberText(std::string_view s, double* v) {
    if (!LooksLikeNumber(s)) return false;
    const auto r = std::from_chars(s.data(), s.data() + s.size(), *v);
    return r.ec == std::errc() && r.ptr == s.data() + s.size() && std::isfinite(*v);
}

bool ParseConsoleValue(std::string_view text, Json* out, std::string* error) {
    if (text.empty()) {
        *out = Json("");
        return true;
    }
    const char c = text[0];
    if (c == '"' || c == '[' || c == '{') {
        std::string e;
        if (!Json::Parse(text, out, &e)) {
            *error = "bad value '" + std::string(text) + "': " + e;
            return false;
        }
        return true;
    }
    if (text == "true" || text == "false") {
        *out = Json(text == "true");
        return true;
    }
    if (text == "null") {
        *out = Json();
        return true;
    }
    double v = 0.0;
    if (ParseNumberText(text, &v)) {
        *out = Json(v);
        return true;
    }
    if (text.find(',') != std::string_view::npos) {
        Json arr = Json::Array();
        size_t start = 0;
        bool all_numbers = true;
        while (start <= text.size()) {
            const size_t comma = text.find(',', start);
            const std::string_view part = text.substr(start, comma == std::string_view::npos ? std::string_view::npos : comma - start);
            double pv = 0.0;
            if (!ParseNumberText(part, &pv)) {
                all_numbers = false;
                break;
            }
            arr.Push(pv);
            if (comma == std::string_view::npos) break;
            start = comma + 1;
        }
        if (all_numbers) {
            *out = std::move(arr);
            return true;
        }
    }
    *out = Json(std::string(text));
    return true;
}

bool BareWordSafe(std::string_view s) {
    if (s.empty() || s == "true" || s == "false" || s == "null") return false;
    for (char c : s) {
        const bool ok = IsIdentChar(c) || c == '.' || c == '/' || c == '\\' || c == ':' || c == '@' || c == '+' || c == '-';
        if (!ok) return false;
    }
    double v = 0.0;
    return !ParseNumberText(s, &v);
}

std::string FormatConsoleValue(const Json& v) {
    switch (v.type()) {
        case Json::Type::kString:
            return BareWordSafe(v.AsString()) ? v.AsString() : v.Dump();
        case Json::Type::kArray: {
            if (v.size() >= 2 && std::all_of(v.items().begin(), v.items().end(), [](const Json& j) { return j.is_number() && std::isfinite(j.AsNumber()); })) {
                std::string s;
                for (size_t i = 0; i < v.size(); ++i) {
                    if (i) s.push_back(',');
                    s += FormatNumber(v.at(i).AsNumber());
                }
                return s;
            }
            return v.Dump();
        }
        default:
            return v.Dump();
    }
}

}  // namespace

bool ParseConsoleLine(std::string_view line, std::string* cmd, Json* args, std::string* error) {
    std::string err;
    std::vector<std::string> tokens;
    if (!SplitTokens(line, &tokens, &err)) {
        if (error) *error = err;
        return false;
    }
    if (tokens.empty()) {
        if (error) *error = "an empty line";
        return false;
    }
    const std::string& name = tokens[0];
    if (name.empty() || !(name[0] >= 'a' && name[0] <= 'z') ||
        !std::all_of(name.begin(), name.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_'; })) {
        if (error) *error = "'" + name + "' is not a command name (lower case words joined by dots)";
        return false;
    }
    // The `key=value` tokens first: a bare word then takes the next schema argument that no `key=` named.
    struct Item {
        std::string key;
        std::string value;
        bool has_key = false;
    };
    std::vector<Item> items;
    std::vector<std::string> used;
    for (size_t i = 1; i < tokens.size(); ++i) {
        const std::string& t = tokens[i];
        size_t p = 0;
        while (p < t.size() && IsIdentChar(t[p])) ++p;
        Item it;
        if (p > 0 && p < t.size() && t[p] == '=' && IsIdentStart(t[0])) {
            it.has_key = true;
            it.key = t.substr(0, p);
            it.value = t.substr(p + 1);
            used.push_back(it.key);
        } else {
            it.value = t;
        }
        items.push_back(std::move(it));
    }
    Schema schema;
    const bool known = GlobalEngine().SchemaOf(name, &schema);
    size_t next_arg = 0;
    Json result = Json::Object();
    for (const Item& it : items) {
        std::string key = it.key;
        if (!it.has_key) {
            if (!known) {
                if (error) *error = "'" + name + "' is not a known command, so '" + it.value + "' has no argument to go to: write key=value";
                return false;
            }
            while (next_arg < schema.args.size() && std::find(used.begin(), used.end(), schema.args[next_arg].name) != used.end()) ++next_arg;
            if (next_arg >= schema.args.size()) {
                if (error) *error = name + ": too many bare values ('" + it.value + "')";
                return false;
            }
            key = schema.args[next_arg++].name;
        }
        Json value;
        if (!ParseConsoleValue(it.value, &value, &err)) {
            if (error) *error = err;
            return false;
        }
        result.Set(key, std::move(value));
    }
    *cmd = name;
    *args = std::move(result);
    return true;
}

std::string FormatConsoleLine(std::string_view cmd, const Json& args) {
    std::string s(cmd);
    if (!args.is_object()) return s;
    for (const Json::Member& m : args.members()) {
        s.push_back(' ');
        s += m.first;
        s.push_back('=');
        s += FormatConsoleValue(m.second);
    }
    return s;
}

}  // namespace mc::cinematic::director
