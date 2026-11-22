#pragma once
//
// --join_server on the game's side (join_server_core.h says what the argument is): once the city is up it asks the relay about the
// target (is it there, does it need mods this game lacks, is the host's room open), then hands a join to the server browser's join
// (browser_join.h) and follows it to the end. Every step is a [join_server] line in the log and a line of
// the log folder's join_server.txt; a failure also opens the server browser with the sentence in it.
//

#include <string>

namespace larecomp::browser {

// Guest thread, once per frame (TickServerBrowserJoin). The argument is read on the first call; with none it is a switch on a flag.
void JoinServerTick();

// The browser's join says how it ended (browser_join.cpp, Finish): failed with its reason word and the sentence it showed, or
// handed over (the game joined, or hosts a session). Ignored unless --join_server started that join. Guest thread.
void JoinServerNoteFinish(bool failed, const std::string& reason, const std::string& text);

// Why --join_server could not join; empty when it did not fail, or the player dismissed it. The browser window shows it. Any thread.
std::string JoinServerFailureText();
void JoinServerClearFailure();

}  // namespace larecomp::browser
