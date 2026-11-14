#pragma once
//
// Online Tournament: the rlUpload calls of the tournament menu, answered by
// our backend. The Rate My Ride hooks own the hooked functions (one hook per
// address) and hand these calls over first.
//

#include <cstdint>

namespace tournament::game {

// ReadRecordsByGamer (sub_824F6EA0) on the table "tournament". True when it
// was ours and is now queued.
bool OnReadRecords(uint32_t table, uint32_t max_records, uint32_t records, uint32_t count_out,
                   uint32_t status);

// DownloadFile (sub_824F7578) of a tournament file id. True when served.
bool OnDownload(uint32_t file_id, uint32_t buffer, uint32_t buffer_size, uint32_t size_out,
                uint32_t progress);

}  // namespace tournament::game
