#include "run_ready.h"

#include <cstdio>
#include <limits>
#include <string_view>

int main() {
    using namespace mc::run_ready;
    int checks = 0;
    int failed = 0;
    const auto check = [&](bool success, const char* description) {
        ++checks;
        if (!success) {
            ++failed;
            std::fprintf(stderr, "FAIL %s\n", description);
        }
    };

    Marks marks;
    const Event events[] = {Event::Title, Event::Save, Event::Roam, Event::Race};
    const char* names[] = {"title_ready", "save_loaded", "free_roam", "race_started"};
    for (unsigned i = 0; i < 4; ++i) {
        check(Name(events[i]) == std::string_view(names[i]), names[i]);
        check(marks.Pending(events[i]), "events are independent");
        check(marks.Claim(events[i]), "first emission");
        check(!marks.Pending(events[i]) && !marks.Claim(events[i]), "emit once");
    }

    TitleGate title;
    check(!title.Entered() && !title.ReadyOnPoll(), "input before prompt draw");
    title.Draw();
    check(title.ReadyOnPoll(), "input after prompt draw");
    title.Enter();
    check(title.Entered() && !title.ReadyOnPoll(), "suppress later prompt");
    title.Draw();
    check(!title.ReadyOnPoll(), "another draw after entering");

    RaceClock race;
    check(!race.Tick(0, 0) && !race.Tick(0, 10), "ignore clock outside a race");
    check(!race.Tick(1, -1) && !race.Tick(1, 10), "ignore invalid countdown sample");
    check(!race.Tick(1, std::numeric_limits<float>::quiet_NaN()), "ignore invalid race timer");
    check(!race.Tick(1, 9.29f), "countdown still in progress");
    check(race.Tick(1, 9.3f), "arcade boundary reached");
    check(!race.Tick(2, 12), "new race needs its own countdown");
    check(!race.Tick(2, 0) && !race.Tick(0, 20) && !race.Tick(2, 20), "reset after leaving race");

    std::printf("%d checks, %d failed\n", checks, failed);
    return failed != 0;
}
