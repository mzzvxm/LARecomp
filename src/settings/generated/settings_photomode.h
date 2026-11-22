#pragma once
// Written by merge-tools/wpl/cvar_convert.py from LARecomp's REXCVAR_DEFINE_* lines. From here on it is
// plain source: edit it like any other header. All owners share the scope "larecomp" and the members keep
// the names players know, so `larecomp.<name>` is the full name and `<name>` stays an alias (RegisterAll).

#include <string>

#include <rex/setting.h>
#include <rex/types.h>
#include "settings/immortal.h"

namespace larecomp::settings {

class PhotoModeSettings final : public rex::SettingOwner {
 public:
  PhotoModeSettings() : SettingOwner("larecomp") {}

  REX_SETTING(bool, photo_album_export, true, "The photo album's PUBLISH ALBUM button becomes EXPORT ALBUM: it saves every photo "
    "in the album as a JPEG in photo_album_export_dir instead of uploading the album to "
    "the Social Club (closed). Photos already in the folder are not written twice.");

  REX_SETTING(std::string, photo_album_export_dir, "", "Folder EXPORT ALBUM writes the photos to. Empty = Pictures\\LARecomp.");

  REX_SETTING(bool, photo_album_keep_new_preview, true, "Fix: while choosing the slot for a new photo, the album shows the photo you just "
    "took -- in the big picture and pulsing in the highlighted slot -- instead of the "
    "last saved photo the cursor passed over. The game decodes every photo into one "
    "shared preview image, and in the slot picker it let the slots overwrite the new "
    "one.", rex::kDebugOnly);

  REX_SETTING(bool, photo_auto_readback, true, "Fix: photo mode previews show an old frame or garbage. The game takes its "
    "picture on the CPU, by locking the front buffer and JPEG-encoding it, so it "
    "needs the GPU resolve copied back to guest memory. This asks for a readback "
    "of just the two front buffers for a few frames around each shot, instead of "
    "the global readback_resolve cvar which stalls every resolve of every frame "
    "(on an SDK without the scoped readback, the global one for about a second "
    "around each shot). Leave ON.", rex::kDebugOnly);

  REX_SETTING(bool, photo_readback_debug, false, "Diagnostics for photo_auto_readback: logs the photo album state machine's "
    "phase transitions and the front buffer addresses the readback is armed for. "
    "Pair with the GPU-side gpu_log_resolve_readback_misses to see whether the "
    "resolve that fills the front buffer actually lands in the armed range.", rex::kDebugOnly);

};

inline PhotoModeSettings& g_photomode = larecomp::Leak<PhotoModeSettings>();

inline auto& photo_album_export = g_photomode.photo_album_export;
inline auto& photo_album_export_dir = g_photomode.photo_album_export_dir;
inline auto& photo_album_keep_new_preview = g_photomode.photo_album_keep_new_preview;
inline auto& photo_auto_readback = g_photomode.photo_auto_readback;
inline auto& photo_readback_debug = g_photomode.photo_readback_debug;

}  // namespace larecomp::settings
