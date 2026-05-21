#pragma once

#include <cstdint>
#include <string>

namespace pesession {

struct ConvertReport {
    int items          = 0;     // total items in the source index
    int blocks         = 0;     // <ShowPlanXML> snapshots written
    int statements     = 0;
    int runtime_rows   = 0;     // .liveexecution aggregates
    int trace_events   = 0;     // TraceRowEx records
    int waits          = 0;
    int64_t in_size    = 0;     // source pesession bytes on disk
    int64_t out_size   = 0;     // osession bytes on disk after close
    double elapsed_s   = 0.0;   // wall time spent converting
    std::string error;          // empty on success
};

// Lossless pesession to osession conversion. Output passes
// verify_lossless byte-for-byte. Removes any pre-existing output
// file first. Throws nothing; failures land in report.error.
// Safe to call from a background thread.
ConvertReport convert_pesession_to_osession(const std::string& in_path,
                                            const std::string& out_path);

}  // namespace pesession
