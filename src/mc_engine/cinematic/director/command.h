#pragma once
//
// The Director's engine room (DIRECTOR-DESIGN.md section 2.1): the registry of commands, the queue they arrive in, the dispatch with its argument
// check, the deferred replies, `batch`, `help`, the event fan-out, the log of recent commands, and the wire's line forms. Pure: no game, no sockets,
// no log, a clock that tests can replace, so larecomp_director_test drives all of it on its own. director.h is the public face; this header is what
// director_runtime.cpp (the game side), line_server.cpp (the sockets) and the tests use.
//
// Threads: Register / Post / Publish / the snapshot / Recent are safe from any thread. RunQueued and PollDeferred, and every handler, run on the game
// thread (director::Tick); a Responder is only ever used there.
//

#include <atomic>
#include <array>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "director.h"

namespace mc::cinematic::director {

class Engine;

namespace detail {
struct ReplySlot {
    Request request;
    std::function<void(const Reply&)> on_reply;
    Engine* engine = nullptr;
    bool finished = false;
    bool deferred = false;
    bool queued_for_poll = false;
    double deadline = 0.0;  // engine clock seconds
    std::function<void(const Responder&)> poll;
    Reply reply;
};
}  // namespace detail

// The events the protocol defines, for `help`.
struct EventDoc {
    const char* name;
    const char* doc;
};
const std::vector<EventDoc>& KnownEvents();

class Engine {
  public:
    using NowFn = std::function<double()>;  // seconds, monotonic
    explicit Engine(NowFn now = {});

    // ---- registry
    bool Register(std::string_view name, Handler handler, Schema schema);
    bool Has(std::string_view name) const;
    bool SchemaOf(std::string_view name, Schema* out) const;
    std::vector<std::string> Names() const;  // sorted
    // `help`'s answer: every command, or the one named.
    Json HelpJson(std::string_view cmd) const;

    // ---- intake (any thread)
    void SetEnabled(bool on);  // off: Post fails; queued commands are answered with `refused`
    bool enabled() const { return enabled_.load(std::memory_order_acquire); }
    bool Post(Posted command);
    size_t queued() const;

    // ---- the game thread
    struct RunStats {
        int ran = 0;
        size_t left = 0;
    };
    // Runs queued commands: at most `max_commands`, and no new one once `budget_ms` has gone by (at least one runs when any is queued).
    RunStats RunQueued(int max_commands = kMaxCommandsPerTick, double budget_ms = kTickBudgetMs);
    // Calls the poll of every deferred reply once; answers the ones that timed out.
    void PollDeferred();
    size_t deferred() const { return deferred_.size(); }
    // Runs one request now, as a handler would see it (tests, the panel's console, `batch`): `on_reply` gets the reply (when it is finished: possibly later).
    std::shared_ptr<detail::ReplySlot> RunNow(Request request, std::function<void(const Reply&)> on_reply);

    // ---- events
    void Publish(std::string_view name, Json fields = Json::Object());
    int AddListener(EventListener listener);
    void RemoveListener(int token);
    // The sink the line server installs: gets the whole event line (no newline) and the event's name; it applies each client's filter with ShouldSend.
    using EventSink = std::function<void(const std::string& line, std::string_view name)>;
    void SetEventSink(EventSink sink);
    // The per-connection event filter (`events.set`).
    bool ShouldSend(int client, std::string_view event_name) const;
    void ResetClientFilter(int client);

    // ---- the snapshot
    void SetSnapshot(std::shared_ptr<const Snapshot> snapshot);
    std::shared_ptr<const Snapshot> GetSnapshot() const;

    // ---- recent commands
    std::vector<LoggedCommand> Recent(size_t max_count) const;

    // ---- extra fields `ping` adds (the game side: frame, scene time, state)
    void SetPingFields(std::function<void(Json&)> fn);

    // Seconds on the engine's clock.
    double Now() const { return now_(); }

  private:
    friend class Responder;
    struct Entry {
        Handler handler;
        Schema schema;
    };
    void RegisterBuiltins();
    void Dispatch(const std::shared_ptr<detail::ReplySlot>& slot);
    void Finish(detail::ReplySlot& slot);
    void LogCommand(const detail::ReplySlot& slot);
    void RunBatch(const Request& request, const Responder& responder);

    NowFn now_;
    std::atomic<bool> enabled_{false};
    std::atomic<uint64_t> next_seq_{1};

    mutable std::mutex registry_mutex_;
    std::map<std::string, Entry, std::less<>> commands_;

    struct Queued {
        Posted posted;
        uint64_t seq = 0;
    };
    mutable std::mutex queue_mutex_;
    std::deque<Queued> queue_;

    std::vector<std::shared_ptr<detail::ReplySlot>> deferred_;  // game thread

    mutable std::mutex events_mutex_;
    std::map<int, EventListener> listeners_;
    int next_listener_ = 1;
    EventSink sink_;
    struct ClientFilter {
        bool on = true;
        bool all = true;
        std::vector<std::string> only;
    };
    std::array<ClientFilter, kMaxClients> filters_;

    mutable std::mutex snapshot_mutex_;
    std::shared_ptr<const Snapshot> snapshot_;

    mutable std::mutex log_mutex_;
    std::deque<LoggedCommand> log_;

    std::function<void(Json&)> ping_fields_;
};

// The one engine of the process.
Engine& GlobalEngine();
// The snapshot contributors added so far (AddSnapshotContributor), in the order they were added.
std::vector<SnapshotContributor> SnapshotContributors();

// ---------------------------------------------------------------------------------------------------------------- the wire's line forms

// {"id":7,"cmd":"camera.set","args":{...},"wait":"settled","timeout_s":20} -> a Posted (source kAgent, on_reply left empty). The reply to a request that fails
// here has the id when one could be read: `*id_out` / `*has_id_out` give it. `error` is the bad_args message.
bool ParseRequest(const Json& object, Posted* out, std::string* error, int64_t* id_out, bool* has_id_out);
// One reply / one event as a line without its newline.
std::string ReplyLine(const Reply& reply);
std::string EventLine(std::string_view name, const Json& fields);

// What the Director's own commands say about a source and a console line, for the strip.
bool IsQuietCommand(std::string_view name);

}  // namespace mc::cinematic::director
