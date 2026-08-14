#ifndef REXGLUE_HAS_XEO3_TARGET
//
// Online Tournament (mcOnlineTournament) hooks.
//
// The tournament's data path is a dead Rockstar LSP stats/sake read, so the
// stock menu shows "server not available". These hooks synthesize the tournament
// locally. Status: it appears and Start passes eligibility, but the actual race
// is a scripted, parameterized, online-context race whose data is empty
// standalone, so launching still hits a null AI brain. WIP — see the RE map in
// the project memory (project_mcla_online_tournament).
//
// Registered as midasm hooks in larecomp_config.toml:
//   sub_82740958  Hook_TournamentInject
//   sub_82389668  Hook_TournamentRaceLookup
//   sub_822BC658  Patch_BrainRacingResetNullGuard
//

#include "online_common.h"

#include <atomic>

// Sentinel written into the synthetic tournament's race-id slot (obj+64). It is
// not a real race hash, so Hook_TournamentRaceLookup can recognise it at the
// city race-table lookup and swap in a race the current city actually has.
constexpr uint32_t kTourneyRaceSentinel = 0x544F5552;  // 'TOUR'

// Online Tournament fake (sub_82740958, mcOnlineTournament state machine).
// r3 = the state object (menu_form + 249*4). The real flow drives a Rockstar LSP
// stats read over a socket whose server is long dead: the read task completes
// with status=2 (fail) at state 7->8, so state 8 falls through to 11 =
// OT_NoLSPConnection ("server not available"). Confirmed live: state=8 status=0x2.
//
// Rather than reimplement the dead LSP wire, we short-circuit the state machine.
// Every tournament field the menu form and eligibility check read lives in the
// sake struct at obj+48 (Version/TableId/RestrictionType/RestrictionData/race id/
// Description), so we populate that and jump straight to state 10 ("found").
//   obj+48 Version   obj+52 TableId   obj+56 RestrictionType (car class)
//   obj+60 RestrictionData (0xFFFFFFFF = wildcard -> eligibility always passes)
//   obj+64 race id (validated by sub_82389668 on Start; 0 = appears but WrongRace)
//   obj+68 Description[1536] (localized name blob, ':'-delimited per language)
// Returns true to skip the original body once we've forced the result.
bool Hook_TournamentInject(PPCRegister& r3) {
    auto* rt = rex::Runtime::instance();
    if (!rt) return false;
    auto* mem = rt->memory();
    if (!mem) return false;
    uint32_t obj = static_cast<uint32_t>(r3.u64);
    if (!IsGuestPtr(obj)) return false;

    uint32_t state = GuestRead32(mem, obj + 4);

    static uint32_t last_state = 0xFFFFFFFF;
    if (state != last_state) {
        last_state = state;
        LARECOMP_APP_INFO("[tourney] state={} -> injecting at 7/8", state);
    }

    // States 7/8 are the read+download that hit the dead LSP. Intercept there,
    // synthesize the tournament, and mark it found.
    if (state == 7 || state == 8) {
        GuestWrite32(mem, obj + 48, 1);           // Version
        GuestWrite32(mem, obj + 52, 13);          // TableId
        GuestWrite32(mem, obj + 56, 0);           // RestrictionType (0 = none)
        GuestWrite32(mem, obj + 60, 0xFFFFFFFF);  // RestrictionData = wildcard
        GuestWrite32(mem, obj + 64, kTourneyRaceSentinel);  // race id sentinel; see
                                                            // Hook_TournamentRaceLookup
        // Description blob: leave as-is for v1; a proper ':'-delimited localized
        // name goes here once appearance is confirmed.
        GuestWrite32(mem, obj + 4, 10);           // state = found
        LARECOMP_APP_INFO("[tourney] injected synthetic tournament, state->10");
        return true;  // skip original state-machine body
    }
    return false;
}

// RAGE string hash, a faithful port of guest sub_821C9790 (used for race-name
// keys). Lowercases A-Z and folds '\\' to '/', then runs the per-char mix.
static uint32_t RageHash(const char* s) {
    uint32_t h = 0;
    for (; *s; ++s) {
        uint8_t c = static_cast<uint8_t>(*s);
        if (c >= 'A' && c <= 'Z')
            c += 32;
        else if (c == '\\')
            c = '/';
        uint32_t v = static_cast<uint32_t>(c) + h;
        uint32_t m = 1025u * v;
        h = (m >> 6) ^ m;
    }
    uint32_t m2 = 9u * h;
    return 32769u * ((m2 >> 11) ^ m2);
}

// Look up `key` in a RAGE hash map at guest address `map` (+0 = bucket array
// ptr, +4 = u16 bucket count; each entry is {key, value, next}). Returns the
// entry address (or 0), matching guest sub_826BDDB0.
static uint32_t RaceMapFind(rex::memory::Memory* mem, uint32_t map, uint32_t key) {
    uint16_t buckets = GuestRead16(mem, map + 4);
    uint32_t bptr = GuestRead32(mem, map + 0);
    if (!buckets || !IsGuestPtr(bptr))
        return 0;
    uint32_t entry = GuestRead32(mem, bptr + 4u * (key % buckets));
    while (IsGuestPtr(entry)) {
        if (GuestRead32(mem, entry + 0) == key)
            return entry;
        entry = GuestRead32(mem, entry + 8);
    }
    return 0;
}

// Tournament race resolver (sub_82389668, the city race-table lookup the
// tournament eligibility check runs). r3 = race manager (city config), r4 = race
// id key. The synthetic tournament stores kTourneyRaceSentinel as its race id;
// no real race matches, so without help the menu reports "WRONG RACE".
//
// Picking just any race is wrong: the first city-map entry turned out to be a
// scripted race that drives an AI "brain" (BrainRacing_Reset), which is null
// outside its scripted context and crashes. The real tournament races are named
// "<district>_tournament1" (hw/hl/be/dt). We hash those names the same way the
// game does and, if one is present in this city's race map, use it — a genuine
// tournament circuit with no scripted-brain dependency. The diagnostic log
// reports what the map holds so the choice can be verified on the target.
void Hook_TournamentRaceLookup(PPCRegister& r3, PPCRegister& r4) {
    if (static_cast<uint32_t>(r4.u64) != kTourneyRaceSentinel) return;
    auto* rt = rex::Runtime::instance();
    if (!rt) return;
    auto* mem = rt->memory();
    if (!mem) return;
    uint32_t race_mgr = static_cast<uint32_t>(r3.u64);
    if (!IsGuestPtr(race_mgr)) return;

    static const char* const kTourneyRaceNames[] = {
        "hw_tournament1", "hl_tournament1", "be_tournament1", "dt_tournament1"};

    // The "<district>_tournament1" event lives in the event map at
    // (*dword_8286D8DC)+8456, but its value is a tournament DEFINITION (a list of
    // races), not a loadable race -- feeding it to Start crashed. The definition
    // holds the actual race entries; guest sub_826CD180(def, idx) returns the
    // current entry and its race name sits at entry+10. We hash that name and use
    // it in the CITY map (race_mgr+52), so eligibility resolves the tournament's
    // real race with the correct object layout and Start loads it. No r3 redirect.
    uint32_t tmgr = GuestRead32(mem, 0x8286D8DC);
    if (IsGuestPtr(tmgr)) {
        uint32_t tmap = tmgr + 8456;
        for (const char* name : kTourneyRaceNames) {
            uint32_t def_entry = RaceMapFind(mem, tmap, RageHash(name));
            if (!def_entry) continue;
            uint32_t def = GuestRead32(mem, def_entry + 4);  // tournament definition
            if (!IsGuestPtr(def)) continue;

            // sub_826CD180(def, 0): array=*(def+0), count=*(u16)(def+4),
            // idx=*(def+32); entry = array[clamp(idx)] (or array[0] if idx<0).
            uint16_t count = GuestRead16(mem, def + 4);
            uint32_t array = GuestRead32(mem, def + 0);
            int32_t idx = static_cast<int32_t>(GuestRead32(mem, def + 32));
            // Diagnostic dump of the tournament definition layout so we can see
            // whether it is populated and whether our offsets are right.
            LARECOMP_APP_INFO(
                "[tourney] def '{}' @0x{:08X}: count={} array=0x{:08X} idx={} "
                "d8=0x{:08X} d12=0x{:08X} d16=0x{:08X}",
                name, def, count, array, idx, GuestRead32(mem, def + 8),
                GuestRead32(mem, def + 12), GuestRead32(mem, def + 16));
            if (!count || !IsGuestPtr(array)) continue;
            if (idx < 0) idx = 0;
            else if (idx > count - 1) idx = count - 1;
            uint32_t entry = GuestRead32(mem, array + 4u * static_cast<uint32_t>(idx));
            if (!IsGuestPtr(entry)) continue;

            std::string race_name = GuestCStr(mem, entry + 10, 64);
            uint32_t rkey = RageHash(race_name.c_str());
            bool in_city = RaceMapFind(mem, race_mgr + 52, rkey) != 0;
            LARECOMP_APP_INFO(
                "[tourney] '{}' entry@0x{:08X} type={} name='{}' key=0x{:08X} in-city-map={}",
                name, entry, GuestRead32(mem, entry + 4), race_name, rkey, in_city ? 1 : 0);
            if (in_city) {
                r4.u64 = rkey;  // real city race for this tournament
                return;
            }
        }
        LARECOMP_APP_INFO("[tourney] could not resolve a tournament race to a city race");
    } else {
        LARECOMP_APP_INFO("[tourney] tournament manager (dword_8286D8DC) is null");
    }

    // Fallback: first city race. Diagnostic only -- likely a scripted race that
    // still crashes, but the log tells us the map state.
    const uint32_t map = race_mgr + 52;
    uint16_t buckets = GuestRead16(mem, map + 4);
    uint32_t bptr = GuestRead32(mem, map + 0);
    int total = 0;
    uint32_t first_key = 0;
    if (buckets && IsGuestPtr(bptr)) {
        for (uint16_t i = 0; i < buckets; ++i) {
            uint32_t e = GuestRead32(mem, bptr + 4u * i);
            while (IsGuestPtr(e)) {
                if (!first_key) first_key = GuestRead32(mem, e + 0);
                ++total;
                e = GuestRead32(mem, e + 8);
            }
        }
    }
    LARECOMP_APP_INFO("[tourney] fallback: city map {} races, first=0x{:08X}", total, first_key);
    if (first_key)
        r4.u64 = first_key;
}

// Null-brain guard for the Scaleform native command "BrainRacing_Reset"
// (sub_822BC658). The command does brain = args[0]; brain->vtable[7](); when the
// tournament race runs a scripted race whose AI brain was never created (no
// online/scripted context standalone), args[0] is null and the deref crashes in
// the per-frame HUD update. r3 = the native-call context; args = *(r3+8), brain
// = *args. Returning true skips the command (return_on_true) when the brain is
// null, leaving the rest of the frame intact; a real brain runs it normally.
bool Patch_BrainRacingResetNullGuard(PPCRegister& r3) {
    auto* rt = rex::Runtime::instance();
    if (!rt) return false;
    auto* mem = rt->memory();
    if (!mem) return false;
    uint32_t ctx = static_cast<uint32_t>(r3.u64);
    if (!IsGuestPtr(ctx)) return false;
    uint32_t args = GuestRead32(mem, ctx + 8);
    if (!IsGuestPtr(args)) return true;
    uint32_t brain = GuestRead32(mem, args);
    if (!IsGuestPtr(brain)) {
        static std::atomic_bool logged{false};
        if (!logged.exchange(true))
            LARECOMP_APP_INFO("[tourney] BrainRacing_Reset on null brain -> skipped");
        return true;  // skip the null deref
    }
    return false;
}

#else // REXGLUE_HAS_XEO3_TARGET
// XEO3 stubs: empty implementations so the linker resolves codegen calls.

#include <rex/ppc/context.h>

bool Hook_TournamentInject(PPCRegister& r3) { return false; }
void Hook_TournamentRaceLookup(PPCRegister& r3, PPCRegister& r4) {}
bool Patch_BrainRacingResetNullGuard(PPCRegister& r3) { return false; }
#endif // REXGLUE_HAS_XEO3_TARGET
