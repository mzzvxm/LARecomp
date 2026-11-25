#pragma once
// Written by merge-tools/wpl/cvar_convert.py from LARecomp's REXCVAR_DEFINE_* lines. From here on it is
// plain source: edit it like any other header. All owners share the scope "larecomp" and the members keep
// the names players know, so `larecomp.<name>` is the full name and `<name>` stays an alias (RegisterAll).

#include <string>

#include <rex/setting.h>
#include <rex/types.h>
#include "settings/immortal.h"

namespace larecomp::settings {

class DiagSettings final : public rex::SettingOwner {
 public:
  DiagSettings() : SettingOwner("larecomp") {}

  REX_SETTING(bool, fxc_probe, false, "Write the bytes of the two static rage streams' buffer (the effect loader's, at "
    "0x82860C18 and 0x82860C40) to fxc_probe_<n>_<stream>_<count>.bin in the working "
    "directory, once per distinct content, for the first two hundred.\n"
    "\n"
    "Compares what the effect (.fxc) parser is handed on two platforms: the same file should "
    "give the same bytes. Observer only; off by default.", rex::kDebugOnly);

  REX_SETTING(bool, guest_profile, false, "Sample the busiest threads, and photograph every thread when the game stops. "
    "The same thing MCLA_PROFILE=1 turns on, reachable from the config file so a "
    "session need not be launched from a shell to get it. The periodic report says "
    "where frame time goes; the stall dump is the one that matters when a load "
    "never finishes, because the thread the sampler follows is parked in a wait by "
    "then and the stuck one is somebody else. Reports land in logs/profile_*.log.", rex::kDebugOnly, rex::kRequiresRestart);  // was inside a #if in the .cpp: check

  REX_SETTING(bool, inflate_trace, false, "Log every call into zlibInflater's decode step, for the first few hundred.\n"
    "\n"
    "A repacked resource that never arrives leaves no other trace: the streamer "
    "opens the file, reads it, and then either spins in sub_821BC140 or asserts "
    "that the stream is not in XCompress format. Both of those follow from one "
    "number -- how much input the decoder actually consumed on a call -- and "
    "this prints it. Read consecutive lines: input left falling says it ate "
    "something, output left falling says it produced something, and a pair of "
    "identical lines is the decoder refusing to move.", rex::kDebugOnly);

  REX_SETTING(bool, input_probe, false, "Log what the SDK's input system hands the game for users 0 to 3 whenever it changes: "
    "the status, the packet number, the buttons and the sticks. Off by default. Polling it "
    "pumps the same input path the game uses, so it can only make a pad look better, never worse; "
    "if a pad shows nothing here, the game gets nothing either.", rex::kDebugOnly, rex::kRequiresRestart);

  REX_SETTING(std::string, segment_trace, "", "Dump the segment table of every resource whose file name contains this "
    "text, as the streamer allocates it.\n"
    "\n"
    "A resource does not arrive in memory as one block. sub_821E58F0 walks a "
    "table of twelve-byte entries -- destination, size, and the address the "
    "resource was built for -- and either allocates them one by one (each landing "
    "wherever the heap had room) or sums them into a single allocation, with "
    "every size rounded up to 128 either way. Every pointer inside the resource "
    "is then resolved by finding which entry's range it falls in (sub_8217D828) "
    "and adding that entry's own delta.\n"
    "\n"
    "So a buffer that starts in one segment and runs past its end is read as if "
    "the next segment followed it, and from the boundary on the GPU fetches "
    "whatever the heap put there -- a mesh that is perfect on disk and blades on "
    "screen. This prints the boundaries so a rebuilt drawable's buffers can be "
    "held against them.\n"
    "\n"
    "Empty turns it off. `/` dumps every resource. `>N` dumps only the ones "
    "whose segments total N bytes or more, which is the setting that works: a "
    "resource read out of an archive has no name to match -- that is what the "
    "fixup error means by 'Unknown file in an archive' -- and a rebuilt car body "
    "is the only thing in the game past a megabyte. Anything else is matched "
    "against the name.", rex::kDebugOnly);

  REX_SETTING(std::string, stream_trace, "", "Log every resource the streamer is asked for whose path contains this "
    "text, and whether the file was found.\n"
    "\n"
    "The hook sits one instruction after pgStreamer::Open returns inside "
    "sub_821E2940, where the resolved path is still on the stack at r1+0x60 and "
    "r3 holds the handle -- minus one when the file was not found. That pair is "
    "the only thing that separates \"the game never asked for it\" from \"the "
    "game asked and the archive did not have it\", which is the question a car "
    "that loads forever comes down to.\n"
    "\n"
    "Empty turns it off. `resources/vehicle` is the useful setting for a car; "
    "`/` logs every file the game opens, which is thousands of lines.", rex::kDebugOnly);

};

inline DiagSettings& g_diag = larecomp::Leak<DiagSettings>();

inline auto& fxc_probe = g_diag.fxc_probe;
inline auto& guest_profile = g_diag.guest_profile;
inline auto& inflate_trace = g_diag.inflate_trace;
inline auto& input_probe = g_diag.input_probe;
inline auto& segment_trace = g_diag.segment_trace;
inline auto& stream_trace = g_diag.stream_trace;

}  // namespace larecomp::settings
