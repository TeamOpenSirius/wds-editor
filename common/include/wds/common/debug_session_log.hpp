#pragma once

namespace wds::common {

// Session 3aea3b: Windows MSAA hitch diagnosis.
// Writes NDJSON to the Cursor debug log (macOS workspace) and
// %LOCALAPPDATA%/WDS/logs/debug-3aea3b.ndjson (Windows).
// Failure to open is not fatal. Call from the UI thread only.
void debug_session_log(const char* location, const char* message, const char* hypothesis_id,
                       const char* data_json);

}  // namespace wds::common
