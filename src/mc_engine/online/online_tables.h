#pragma once
//
// The game side of online_tables_core.h: writes the stats views and the session property names the game holds in memory to
// <log folder>/online_tables.json, for a launcher or a bug report that has no game to ask.
//

#include "online_tables_core.h"

namespace larecomp::online_tables {

// A GuestReader over the loaded title's memory: false while no title is loaded and for a range the page access does not allow
// reading. Any thread once the title is loaded.
const GuestReader& LiveGuestReader();

// Guest thread, once per frame (hooks.cpp). Waits for the property table to fill (a few seconds into boot), writes the file,
// reads the tables once more a little later in case the game filled more of them in, and then does nothing for the rest of the
// run. A file with the same text is not touched.
void Tick();

}  // namespace larecomp::online_tables
