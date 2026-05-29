#pragma once

#include <showplan/showplan.hpp>

#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace pesession {

// Metadata for a single captured session item, drawn from meta.json plus
// what the showplan XML inside its .queryanalysis blob can tell us.
struct PeSessionMeta {
    int file_number = 0;
    std::string instance;       // hostname / SQL instance
    std::string database;
    std::string login;
    std::string created_utc;
    std::string total_time;     // "HH:MM:SS.sss"
    int64_t actual_rows = 0;
    char plan_type = '?';       // 'E' = estimated, 'A' = actual
    int xml_block_count = 0;    // distinct <ShowPlanXML> blocks observed
};

class PeSessionError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

bool is_pesession_file(const std::string& path);

// Decode meta.json. No .queryanalysis is extracted. Returned items
// carry no xml_block_count yet; that field gets populated by
// read_pesession_item.
std::vector<PeSessionMeta> read_pesession_index(const std::string& path);

// Lazily extract and parse a single item by file_number. The ZIP is opened
// and closed inside the call. Throws PeSessionError if the entry is
// missing, oversized, or unreadable; throws showplan::ParseError if no
// usable plan is found inside.
std::unique_ptr<showplan::Plan> read_pesession_item(const std::string& path,
                                                    int file_number,
                                                    int* out_xml_blocks);

// Connection identity extracted from the .queryanalysis NRBF stream's
// Intercerve.SqlServer.ConnectionParameters block. Lossy fields the
// pesession doesn't reliably populate (login, EngineEdition) are
// captured as empty strings.
struct PeSessionConnection {
    std::string server_name;
    std::string database_name;
    std::string auth_type;       // "Not Specified" / "Windows" / "SQL Server" / ...
    std::string login;
    std::string server_version;  // e.g. "15.00.4445"
    bool use_integrated_security = false;
};

[[deprecated("Use read_pesession_item_payload, which extracts and parses "
             "the queryanalysis blob once for all four consumers.")]]
PeSessionConnection read_pesession_connection_params(const std::string& path,
                                                     int file_number);

// Aggregated per-statement runtime metrics keyed by StatementStartOffset.
struct StatementRuntimeAgg {
    int64_t statement_start_offset = -1;
    int64_t row_count = 0;
    int64_t elapsed_ms = 0;
    int64_t cpu_ms = 0;
    int64_t logical_reads = 0;
    int64_t physical_reads = 0;
    int64_t read_aheads = 0;
    int64_t write_pages = 0;
    int64_t end_of_scan_count = 0;
    int64_t rebind_count = 0;
};

// Reads the matching `{file_number}.liveexecution` if present and walks
// every PlanQueryProfileAggregate, aggregating per-StatementStartOffset.
// Returns the aggregates sorted by statement_start_offset ascending. If
// no .liveexecution entry exists, returns an empty vector (no throw).
std::vector<StatementRuntimeAgg> read_pesession_runtime(const std::string& path,
                                                        int file_number);

// Per-statement runtime data captured during an XEvents-style trace (the
// TraceRowEx instances inside .queryanalysis). Durations are stored in
// microseconds matching the SQL Server XEvents convention; convert to ms
// at display time.
struct TraceMetrics {
    std::string plan_handle_hex;   // empty if null
    int64_t duration_us = 0;
    int64_t cpu_us = 0;
    int64_t udf_duration_us = 0;
    int64_t udf_cpu_us = 0;
    int64_t reads = 0;
    int64_t writes = 0;
    int64_t row_count = 0;
    // .NET DateTime raw 64-bit bits (Ticks since year-1 + Kind in the
    // top 2 bits). 0 means unknown. Helper conversions live next to
    // the statement-table rendering in panels.cpp.
    uint64_t start_dt_raw = 0;
    uint64_t end_dt_raw   = 0;
    // TraceRowEx provenance. nest_level=1 is the outer batch,
    // 2+ inside an EXEC'd sproc.
    std::string object_name;
    int32_t  object_id    = 0;
    int32_t  nest_level   = 0;
    int32_t  line_number  = 0;
    int64_t  offset_bytes = 0;     // StatementStartOffset, UTF-16 bytes
    // Statement text from the trace. EXEC dispatchers without their
    // own ShowPlanXML show up only here.
    std::string text;
};

struct WaitEntry {
    std::string wait_type;
    int64_t total_duration_ms = 0;
    int64_t total_signal_ms = 0;
    // PlanHandle (hex) of the enclosing QueryStats / PlanData, when known.
    // Empty if the WaitAggregate was outside any plan-handle-bearing scope.
    std::string parent_plan_handle_hex;
};

// Full extraction of one session item's .queryanalysis NRBF stream.
// Captures TraceRowEx (per-statement metrics in trace order),
// QueryStats (UDF + plan-handle keyed extras), WaitAggregate (per
// statement, in the order they appear), and PlanData→PlanHandle order
// (for PlanHandle-based matching to plan statements).
struct SessionTraceData {
    std::vector<TraceMetrics> traces;         // one per TraceRowEx, in document order
    std::vector<TraceMetrics> stats;          // one per QueryStats
    std::vector<WaitEntry>    waits;          // session-level (TBD: per-stmt split)
    // PlanHandle hex per PlanData instance, in document order (positionally
    // mirrors statements that came out of the embedded ShowPlanXML scan).
    std::vector<std::string>  plan_handles;
};

// Parses the .queryanalysis entry's NRBF payload (silently returns an
// empty SessionTraceData if the entry is missing or the parse fails).
[[deprecated("Use read_pesession_item_payload, which extracts and parses "
             "the queryanalysis blob once for all four consumers.")]]
SessionTraceData read_pesession_traces(const std::string& path,
                                       int file_number);

// Returns every `<ShowPlanXML>...</ShowPlanXML>` substring found in the
// item's .queryanalysis blob, in document order. Each entry is a
// standalone, parseable XML string suitable for storing directly in an
// osession `plans` row. Returns an empty vector when the item is absent
// or has no embedded plans.
[[deprecated("Use read_pesession_item_payload, which extracts the "
             "queryanalysis blob once for all four consumers + the "
             "ShowPlanXML substring scan.")]]
std::vector<std::string> extract_pesession_xml_blocks(const std::string& path,
                                                      int file_number);

// Batch SQL the user submitted for this item. Sourced from
// QueryAnalyzerContext.traceRowText. Includes GO separators, DBCC
// lines, comments. Empty when traceRowText isn't present.
[[deprecated("Use read_pesession_item_payload, which extracts and parses "
             "the queryanalysis blob once for all four consumers.")]]
std::string read_pesession_batch_text(const std::string& path,
                                      int file_number);

// Raw gzipped JSON from QueryAnalyzerInput.IndexAnalyzerResultsGz.
// Caller decompresses on demand. Empty when no analyzer payload.
[[deprecated("Use read_pesession_item_payload, which extracts and parses "
             "the queryanalysis blob once for all four consumers.")]]
std::string read_pesession_index_analyzer_gz(const std::string& path,
                                              int file_number);

// Fused single-pass read of one item's .queryanalysis blob, replacing the
// four deprecated readers above: opens the archive once, extracts once, and
// runs one nrbf::parse via MultiVisitor. No throw: fields are empty when the
// item is absent or unreadable, and NRBF parse errors are absorbed.
struct PeSessionItemPayload {
    std::vector<std::string> showplan_xml_blocks;
    SessionTraceData         traces;
    PeSessionConnection      connection_params;
    std::string              batch_text;
    std::string              index_analyzer_gz;
};

PeSessionItemPayload read_pesession_item_payload(const std::string& path,
                                                 int file_number);

// Phase-split building blocks for staged loading. read_pesession_item_payload
// is just their composition. A caller that wants the plan before the rest
// extracts the blob once, scans it for ShowPlanXML to render, then parses the
// NRBF on a worker, reusing the same blob (no second decompress).
std::string read_pesession_queryanalysis_blob(const std::string& path,
                                               int file_number);
std::vector<std::string> scan_showplan_blocks(std::string_view blob);
PeSessionItemPayload parse_pesession_queryanalysis(std::string_view blob);

}  // namespace pesession
