#include "pesession/pesession.hpp"

#include <nlohmann/json.hpp>
#include <miniz.h>
#include <nrbf/nrbf.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <unordered_map>

namespace pesession {

namespace {

constexpr size_t kMaxExtractBytes = 2ull * 1024ull * 1024ull * 1024ull;  // 2 GiB per entry
constexpr int    kMaxPlansPerItem = 8192;

struct ZipHandle {
    mz_zip_archive z{};
    bool open = false;
    explicit ZipHandle(const std::string& path) {
        if (mz_zip_reader_init_file(&z, path.c_str(), 0)) {
            open = true;
        }
    }
    ~ZipHandle() { if (open) mz_zip_reader_end(&z); }
};

std::string extract_entry(mz_zip_archive& zip, const char* name) {
    int idx = mz_zip_reader_locate_file(&zip, name, nullptr, 0);
    if (idx < 0) {
        throw PeSessionError(std::string("Missing entry: ") + name);
    }
    mz_zip_archive_file_stat st{};
    if (!mz_zip_reader_file_stat(&zip, idx, &st)) {
        throw PeSessionError(std::string("Cannot stat: ") + name);
    }
    if (st.m_uncomp_size > kMaxExtractBytes) {
        throw PeSessionError(std::string(name) + " exceeds " +
                             std::to_string(kMaxExtractBytes / (1024ull * 1024ull)) +
                             " MiB extraction cap");
    }
    std::string buf;
    buf.resize(static_cast<size_t>(st.m_uncomp_size));
    if (!mz_zip_reader_extract_to_mem(&zip, idx, buf.data(), buf.size(), 0)) {
        throw PeSessionError(std::string("Inflate failed for ") + name);
    }
    return buf;
}

std::vector<std::string_view> extract_showplan_blocks(std::string_view blob) {
    std::vector<std::string_view> out;
    static constexpr std::string_view kOpen  = "<ShowPlanXML";
    static constexpr std::string_view kClose = "</ShowPlanXML>";
    size_t pos = 0;
    while (out.size() < kMaxPlansPerItem) {
        auto a = blob.find(kOpen, pos);
        if (a == std::string_view::npos) break;
        auto b = blob.find(kClose, a + kOpen.size());
        if (b == std::string_view::npos) break;
        size_t end = b + kClose.size();
        out.push_back(blob.substr(a, end - a));
        pos = end;
    }
    return out;
}

std::string opt_string(const nlohmann::json& j, const char* key) {
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return {};
    if (it->is_string()) return it->get<std::string>();
    return it->dump();
}

int64_t opt_int(const nlohmann::json& j, const char* key) {
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return 0;
    if (it->is_number_integer()) return it->get<int64_t>();
    if (it->is_number())         return static_cast<int64_t>(it->get<double>());
    return 0;
}

char plan_type_char(const std::string& s) {
    if (s.empty()) return '?';
    char c = static_cast<char>(std::toupper(static_cast<unsigned char>(s[0])));
    return (c == 'E' || c == 'A') ? c : '?';
}

PeSessionMeta meta_from_json(const nlohmann::json& it) {
    PeSessionMeta m;
    m.file_number   = static_cast<int>(opt_int(it, "FileNumber"));
    m.instance      = opt_string(it, "Instance");
    m.database      = opt_string(it, "Database");
    m.login         = opt_string(it, "Login");
    m.created_utc   = opt_string(it, "CreationDateUtc");
    m.total_time    = opt_string(it, "TotalTime");
    m.actual_rows   = opt_int(it, "ActualRows");
    m.plan_type     = plan_type_char(opt_string(it, "PlanType"));
    return m;
}

}  // namespace

bool is_pesession_file(const std::string& path) {
    std::ifstream f(std::filesystem::u8path(path), std::ios::binary);
    if (!f) return false;
    char hdr[4]{};
    f.read(hdr, 4);
    if (f.gcount() < 4) return false;
    return hdr[0] == 'P' && hdr[1] == 'K' &&
           (hdr[2] == 3 || hdr[2] == 5 || hdr[2] == 7) &&
           (hdr[3] == 4 || hdr[3] == 6 || hdr[3] == 8);
}

std::vector<PeSessionMeta> read_pesession_index(const std::string& path) {
    ZipHandle zh(path);
    if (!zh.open) {
        throw PeSessionError("Not a valid ZIP / .pesession file: " + path);
    }
    std::string meta_text;
    try {
        meta_text = extract_entry(zh.z, "meta.json");
    } catch (PeSessionError&) {
        throw PeSessionError("meta.json not found. Not a Plan Explorer session");
    }
    nlohmann::json doc;
    try {
        doc = nlohmann::json::parse(meta_text);
    } catch (const nlohmann::json::parse_error& e) {
        throw PeSessionError(std::string("meta.json parse error: ") + e.what());
    }
    auto items = doc.find("PlanExplorerSessionItemMetadataItems");
    if (items == doc.end() || !items->is_array()) {
        throw PeSessionError("meta.json missing PlanExplorerSessionItemMetadataItems");
    }
    std::vector<PeSessionMeta> out;
    out.reserve(items->size());
    for (const auto& it : *items) out.push_back(meta_from_json(it));
    return out;
}

namespace {

// .liveexecution carries multiple snapshots over the query's lifetime.
// Each snapshot has one PlanQueryProfileAggregate per operator with
// monotonically-growing cumulative counters. To avoid summing each
// operator N times across N samples, we keep only the LAST seen aggregate
// per (StatementStartOffset, NodeID) pair, then aggregate across operators
// at the end.
class RuntimeVisitor final : public nrbf::Visitor {
public:
    std::unordered_map<int64_t, StatementRuntimeAgg> by_offset;

    bool enter_instance(int32_t /*id*/,
                        const nrbf::ClassDef& def) override {
        if (def.name.find("PlanQueryProfileAggregate") != std::string::npos) {
            current_ = StatementRuntimeAgg{};
            current_node_id_ = -1;
            current_offset_set_ = false;
            collecting_ = true;
        }
        return true;   // descend so nested containers expose their members
    }

    void member(const nrbf::ClassMember& m,
                const nrbf::Value& v) override {
        if (!collecting_) return;
        std::string_view n = m.name;
        if      (n == "<StatementStartOffset>k__BackingField") {
            current_.statement_start_offset = v.i;
            current_offset_set_ = true;
        }
        else if (n == "<NodeID>k__BackingField")          current_node_id_       = static_cast<int>(v.i);
        else if (n == "<RowCount>k__BackingField")        current_.row_count       = v.i;
        else if (n == "<ElapsedTimeMs>k__BackingField")   current_.elapsed_ms      = v.i;
        else if (n == "<CpuTimeMs>k__BackingField")       current_.cpu_ms          = v.i;
        else if (n == "<LogicalReadCount>k__BackingField")  current_.logical_reads  = v.i;
        else if (n == "<PhysicalReadCount>k__BackingField") current_.physical_reads = v.i;
        else if (n == "<ReadAheadCount>k__BackingField")    current_.read_aheads    = v.i;
        else if (n == "<WritePageCount>k__BackingField")    current_.write_pages    = v.i;
        else if (n == "<EndOfScanCount>k__BackingField")    current_.end_of_scan_count = v.i;
        else if (n == "<RebindCount>k__BackingField")       current_.rebind_count   = v.i;
    }

    void exit_instance(int32_t /*id*/,
                       const nrbf::ClassDef& def) override {
        if (!collecting_) return;
        if (def.name.find("PlanQueryProfileAggregate") == std::string::npos) return;
        collecting_ = false;
        if (!current_offset_set_) return;
        // Max-per-field per (offset, node_id) across all snapshots. Each
        // metric counter in a snapshot is "current cumulative within that
        // run". but later snapshots can read zero when the operator has
        // closed, so a plain "last wins" loses data. `max` is robust to
        // both monotonic and zeroed-out tails.
        int64_t key = (current_.statement_start_offset << 16) |
                      (current_node_id_ & 0xFFFF);
        auto& s = per_node_[key];
        s.statement_start_offset = current_.statement_start_offset;
        if (current_.row_count       > s.row_count)       s.row_count       = current_.row_count;
        if (current_.elapsed_ms      > s.elapsed_ms)      s.elapsed_ms      = current_.elapsed_ms;
        if (current_.cpu_ms          > s.cpu_ms)          s.cpu_ms          = current_.cpu_ms;
        if (current_.logical_reads   > s.logical_reads)   s.logical_reads   = current_.logical_reads;
        if (current_.physical_reads  > s.physical_reads)  s.physical_reads  = current_.physical_reads;
        if (current_.read_aheads     > s.read_aheads)     s.read_aheads     = current_.read_aheads;
        if (current_.write_pages     > s.write_pages)     s.write_pages     = current_.write_pages;
        if (current_.end_of_scan_count > s.end_of_scan_count) s.end_of_scan_count = current_.end_of_scan_count;
        if (current_.rebind_count    > s.rebind_count)    s.rebind_count    = current_.rebind_count;
    }

    bool enter_object_array(int32_t, int32_t) override { return true; }
    bool enter_string_array(int32_t, int32_t) override { return false; }
    bool enter_primitive_array(int32_t, nrbf::PrimitiveType, int32_t) override {
        return false;
    }

    void finalize() {
        for (auto& kv : per_node_) {
            const auto& v = kv.second;
            auto& slot = by_offset[v.statement_start_offset];
            slot.statement_start_offset = v.statement_start_offset;
            if (v.row_count > slot.row_count)   slot.row_count = v.row_count;
            if (v.elapsed_ms > slot.elapsed_ms) slot.elapsed_ms = v.elapsed_ms;
            slot.cpu_ms         += v.cpu_ms;
            slot.logical_reads  += v.logical_reads;
            slot.physical_reads += v.physical_reads;
            slot.read_aheads    += v.read_aheads;
            slot.write_pages    += v.write_pages;
            slot.end_of_scan_count += v.end_of_scan_count;
            slot.rebind_count   += v.rebind_count;
        }
    }

private:
    StatementRuntimeAgg current_{};
    int current_node_id_ = -1;
    bool current_offset_set_ = false;
    bool collecting_ = false;
    std::unordered_map<int64_t, StatementRuntimeAgg> per_node_;
};

}  // namespace

namespace {

// True if a class-member name like "<Field>k__BackingField" or
// "TraceRow+<Field>k__BackingField" refers to the auto-property `simple`.
bool field_is(std::string_view name, std::string_view simple) {
    auto open = name.find('<');
    auto close = name.find(">k__BackingField");
    if (open == std::string_view::npos || close == std::string_view::npos) {
        return false;
    }
    return name.substr(open + 1, close - open - 1) == simple;
}

std::string bytes_to_hex(const std::vector<uint8_t>& b) {
    static const char* hex = "0123456789abcdef";
    std::string s;
    s.reserve(b.size() * 2);
    for (uint8_t c : b) {
        s.push_back(hex[c >> 4]);
        s.push_back(hex[c & 0xF]);
    }
    return s;
}

// Captures TraceRowEx / QueryStats / WaitAggregate / PlanData instances
// in document order. Holds a small frame stack so nested instances each
// get their own scratch space.
class TraceVisitor final : public nrbf::Visitor {
public:
    SessionTraceData out;

    enum class Kind { Other, TraceRow, QueryStats, WaitAggregate, PlanData };
    struct Frame {
        Kind kind = Kind::Other;
        TraceMetrics m;
        WaitEntry    w;
        std::string  plan_handle_hex;
        int32_t      pending_wait_type_id = 0;   // unresolved at member-time
        // Same deferred-resolution model as wait types: ObjectName
        // sometimes references a StringObject that streams later.
        int32_t      pending_object_name_id = 0;
        // TraceRow+<TextData>k__BackingField is also a forward-ref
        // string in most pesession streams (Sentry hashes the trace
        // text and writes a shared dictionary entry).
        int32_t      pending_text_id = 0;
    };

    bool enter_instance(int32_t, const nrbf::ClassDef& d) override {
        Frame f;
        if      (d.name.find(".Trace.TraceRowEx") != std::string::npos ||
                 d.name.find(".Trace.TraceRow")   != std::string::npos)
            f.kind = Kind::TraceRow;
        else if (d.name == "Intercerve.SqlServer.Plans.QueryStats")
            f.kind = Kind::QueryStats;
        else if (d.name == "Intercerve.SqlServer.XEvents.WaitAggregate")
            f.kind = Kind::WaitAggregate;
        else if (d.name.find(".Plans.PlanData") != std::string::npos &&
                 d.name.find("+") == std::string::npos)
            f.kind = Kind::PlanData;
        stack_.push_back(std::move(f));
        return true;
    }

    void member(const nrbf::ClassMember& m, const nrbf::Value& v) override {
        current_member_ = m.name;
        if (stack_.empty()) return;
        Frame& top = stack_.back();
        switch (top.kind) {
        case Kind::TraceRow:
            if      (field_is(m.name, "Duration"))  top.m.duration_us = v.i;
            else if (field_is(m.name, "Cpu"))       top.m.cpu_us      = v.i;
            else if (field_is(m.name, "Reads"))     top.m.reads       = v.i;
            else if (field_is(m.name, "Writes"))    top.m.writes      = v.i;
            else if (field_is(m.name, "RowCounts")) top.m.row_count   = v.i;
            else if (field_is(m.name, "StartTimeUtc") &&
                     v.kind == nrbf::Value::Kind::DateTime) {
                top.m.start_dt_raw = v.u;
            }
            else if (field_is(m.name, "EndTimeUtc") &&
                     v.kind == nrbf::Value::Kind::DateTime) {
                top.m.end_dt_raw = v.u;
            }
            // Sproc / nesting provenance. ObjectName is a string in
            // the NRBF stream. may come as an inline String or a
            // forward ObjectRef into the dictionary; both paths
            // resolve via resolve_string() + pending_object_strings_.
            else if (field_is(m.name, "ObjectName")) {
                top.m.object_name = resolve_string(v);
                if (top.m.object_name.empty() &&
                    v.kind == nrbf::Value::Kind::ObjectRef && v.object_id) {
                    top.pending_object_name_id = v.object_id;
                }
            }
            else if (field_is(m.name, "ObjectID"))   top.m.object_id   = static_cast<int32_t>(v.i);
            else if (field_is(m.name, "NestLevel"))  top.m.nest_level  = static_cast<int32_t>(v.i);
            else if (field_is(m.name, "LineNumber")) top.m.line_number = static_cast<int32_t>(v.i);
            else if (field_is(m.name, "Offset"))     top.m.offset_bytes = v.i;
            else if (field_is(m.name, "TextData")) {
                top.m.text = resolve_string(v);
                if (top.m.text.empty() &&
                    v.kind == nrbf::Value::Kind::ObjectRef && v.object_id) {
                    top.pending_text_id = v.object_id;
                }
            }
            break;
        case Kind::QueryStats:
            if      (field_is(m.name, "CpuTime"))      top.m.cpu_us         = v.i;
            else if (field_is(m.name, "Duration"))     top.m.duration_us    = v.i;
            else if (field_is(m.name, "LogicalReads")) top.m.reads          = v.i;
            else if (field_is(m.name, "UdfCpuTime"))   top.m.udf_cpu_us     = v.i;
            else if (field_is(m.name, "UdfDuration"))  top.m.udf_duration_us= v.i;
            break;
        case Kind::WaitAggregate:
            if (field_is(m.name, "WaitType")) {
                top.w.wait_type = resolve_string(v);
                if (top.w.wait_type.empty() &&
                    v.kind == nrbf::Value::Kind::ObjectRef &&
                    v.object_id) {
                    top.pending_wait_type_id = v.object_id;
                }
            } else if (field_is(m.name, "TotalDuration")) {
                top.w.total_duration_ms = v.i;
            } else if (field_is(m.name, "TotalSignalDuration")) {
                top.w.total_signal_ms = v.i;
            }
            break;
        default: break;
        }
    }

    bool enter_primitive_array(int32_t, nrbf::PrimitiveType pt,
                               int32_t len) override {
        if (pt == nrbf::PrimitiveType::Byte &&
            (current_member_.find("PlanHandle") != std::string::npos ||
             current_member_.find("SqlHandle")  != std::string::npos)) {
            byte_buf_.clear();
            byte_buf_.reserve(static_cast<size_t>(std::max(0, len)));
            collecting_bytes_ = true;
            return true;
        }
        return false;
    }

    void primitive_array_value(int32_t, const nrbf::Value& v) override {
        if (collecting_bytes_) byte_buf_.push_back(static_cast<uint8_t>(v.u));
    }

    void exit_primitive_array(int32_t) override {
        if (!collecting_bytes_) return;
        collecting_bytes_ = false;
        if (stack_.empty()) { byte_buf_.clear(); return; }
        bool is_plan = current_member_.find("PlanHandle") != std::string::npos;
        if (is_plan) {
            std::string hex = bytes_to_hex(byte_buf_);
            Frame& top = stack_.back();
            top.m.plan_handle_hex = hex;
            if (top.kind == Kind::PlanData) {
                top.plan_handle_hex = std::move(hex);
            }
        }
        byte_buf_.clear();
    }

    void exit_instance(int32_t, const nrbf::ClassDef&) override {
        if (stack_.empty()) return;
        Frame f = std::move(stack_.back());
        stack_.pop_back();
        switch (f.kind) {
        case Kind::TraceRow:
            {
                size_t idx = out.traces.size();
                out.traces.push_back(f.m);
                if (f.pending_object_name_id != 0) {
                    pending_trace_object_names_.emplace_back(
                        idx, f.pending_object_name_id);
                }
                if (f.pending_text_id != 0) {
                    pending_trace_texts_.emplace_back(
                        idx, f.pending_text_id);
                }
            }
            break;
        case Kind::QueryStats:    out.stats.push_back(f.m); break;
        case Kind::WaitAggregate: {
            // Tag the wait with the nearest enclosing PlanHandle (from a
            // QueryStats or PlanData frame). Empty when the WaitAggregate
            // is at session scope.
            for (auto it = stack_.rbegin(); it != stack_.rend(); ++it) {
                if (it->kind == Kind::QueryStats && !it->m.plan_handle_hex.empty()) {
                    f.w.parent_plan_handle_hex = it->m.plan_handle_hex;
                    break;
                }
                if (it->kind == Kind::PlanData && !it->plan_handle_hex.empty()) {
                    f.w.parent_plan_handle_hex = it->plan_handle_hex;
                    break;
                }
            }
            size_t idx = out.waits.size();
            out.waits.push_back(f.w);
            if (f.pending_wait_type_id != 0) {
                pending_wait_strings_.emplace_back(idx,
                                                   f.pending_wait_type_id);
            }
            break;
        }
        case Kind::PlanData:      out.plan_handles.push_back(f.plan_handle_hex); break;
        default: break;
        }
    }

    void string_object(int32_t id, std::string_view s) override {
        strings_.emplace(id, std::string(s));
        // Resolve any pending forward refs that pointed at this id.
        // Cheap linear scan. wait-aggregate counts are tiny relative
        // to the rest of the trace stream.
        for (auto it = pending_wait_strings_.begin();
             it != pending_wait_strings_.end(); ) {
            if (it->second == id) {
                if (it->first < out.waits.size()) {
                    out.waits[it->first].wait_type.assign(s);
                }
                it = pending_wait_strings_.erase(it);
            } else {
                ++it;
            }
        }
        for (auto it = pending_trace_object_names_.begin();
             it != pending_trace_object_names_.end(); ) {
            if (it->second == id) {
                if (it->first < out.traces.size()) {
                    out.traces[it->first].object_name.assign(s);
                }
                it = pending_trace_object_names_.erase(it);
            } else {
                ++it;
            }
        }
        for (auto it = pending_trace_texts_.begin();
             it != pending_trace_texts_.end(); ) {
            if (it->second == id) {
                if (it->first < out.traces.size()) {
                    out.traces[it->first].text.assign(s);
                }
                it = pending_trace_texts_.erase(it);
            } else {
                ++it;
            }
        }
    }

    bool enter_object_array(int32_t, int32_t) override { return true; }

private:
    std::string resolve_string(const nrbf::Value& v) const {
        if (!v.s.empty()) return v.s;
        if (v.kind == nrbf::Value::Kind::ObjectRef && v.object_id) {
            auto it = strings_.find(v.object_id);
            if (it != strings_.end()) return it->second;
        }
        return {};
    }

    std::vector<Frame> stack_;
    std::string        current_member_;
    bool               collecting_bytes_ = false;
    std::vector<uint8_t> byte_buf_;
    std::unordered_map<int32_t, std::string> strings_;
    // (out.waits index, pending StringObject id) tuples. patched as
    // the matching StringObject records stream in. Forward references
    // are common because SQL Sentry serializes wait types as a shared
    // dictionary at the end of the stream rather than inline.
    std::vector<std::pair<size_t, int32_t>> pending_wait_strings_;
    // Mirror for TraceRow.ObjectName forward references.
    std::vector<std::pair<size_t, int32_t>> pending_trace_object_names_;
    // Mirror for TraceRow.TextData forward references.
    std::vector<std::pair<size_t, int32_t>> pending_trace_texts_;
};

}  // namespace

std::vector<std::string> extract_pesession_xml_blocks(const std::string& path,
                                                      int file_number) {
    std::vector<std::string> out;
    ZipHandle zh(path);
    if (!zh.open) return out;
    char fname[64];
    std::snprintf(fname, sizeof(fname), "%d.queryanalysis", file_number);
    int idx = mz_zip_reader_locate_file(&zh.z, fname, nullptr, 0);
    if (idx < 0) return out;
    std::string blob;
    try { blob = extract_entry(zh.z, fname); }
    catch (const PeSessionError&) { return out; }
    auto views = extract_showplan_blocks(blob);
    out.reserve(views.size());
    for (auto& v : views) out.emplace_back(v);
    return out;
}

SessionTraceData read_pesession_traces(const std::string& path, int file_number) {
    ZipHandle zh(path);
    if (!zh.open) return {};
    char fname[64];
    std::snprintf(fname, sizeof(fname), "%d.queryanalysis", file_number);
    int idx = mz_zip_reader_locate_file(&zh.z, fname, nullptr, 0);
    if (idx < 0) return {};
    std::string blob;
    try { blob = extract_entry(zh.z, fname); }
    catch (const PeSessionError&) { return {}; }
    TraceVisitor v;
    try { nrbf::parse(blob, v); }
    catch (const nrbf::ParseError&) { return std::move(v.out); }
    return std::move(v.out);
}

std::vector<StatementRuntimeAgg> read_pesession_runtime(const std::string& path,
                                                        int file_number) {
    ZipHandle zh(path);
    if (!zh.open) return {};

    char fname[64];
    std::snprintf(fname, sizeof(fname), "%d.liveexecution", file_number);
    int idx = mz_zip_reader_locate_file(&zh.z, fname, nullptr, 0);
    if (idx < 0) return {};

    std::string blob;
    try {
        blob = extract_entry(zh.z, fname);
    } catch (const PeSessionError&) {
        return {};
    }

    RuntimeVisitor v;
    try {
        nrbf::parse(blob, v);
    } catch (const nrbf::ParseError&) {
        return {};
    }
    v.finalize();

    std::vector<StatementRuntimeAgg> out;
    out.reserve(v.by_offset.size());
    for (auto& kv : v.by_offset) out.push_back(kv.second);
    std::sort(out.begin(), out.end(),
              [](const StatementRuntimeAgg& a, const StatementRuntimeAgg& b) {
                  return a.statement_start_offset < b.statement_start_offset;
              });
    return out;
}

std::unique_ptr<showplan::Plan> read_pesession_item(const std::string& path,
                                                    int file_number,
                                                    int* out_xml_blocks) {
    ZipHandle zh(path);
    if (!zh.open) {
        throw PeSessionError("Not a valid ZIP / .pesession file: " + path);
    }
    char fname[64];
    std::snprintf(fname, sizeof(fname), "%d.queryanalysis", file_number);
    std::string blob = extract_entry(zh.z, fname);

    auto blocks = extract_showplan_blocks(blob);
    if (out_xml_blocks) *out_xml_blocks = static_cast<int>(blocks.size());
    if (blocks.empty()) {
        throw showplan::ParseError("No <ShowPlanXML> block found in " +
                                   std::string(fname));
    }
    auto merged = std::make_unique<showplan::Plan>();
    bool any_ok = false;
    std::string last_err;
    for (auto& block : blocks) {
        try {
            auto p = showplan::parse_xml(block);
            if (merged->server_version.empty())
                merged->server_version = std::move(p.server_version);
            if (merged->build.empty())
                merged->build = std::move(p.build);
            for (auto& s : p.statements) {
                merged->statements.push_back(std::move(s));
            }
            any_ok = true;
        } catch (const showplan::ParseError& e) {
            last_err = e.what();
        }
    }
    if (!any_ok) {
        throw showplan::ParseError(
            last_err.empty()
                ? "No parseable plans inside " + std::string(fname)
                : ("All blocks failed: " + last_err));
    }
    return merged;
}

namespace {

// Minimal NRBF visitor that pulls QueryAnalyzerContext.traceRowText.
// Two-pass via deferred ObjectRefs because the StringObject record for
// the value can stream after the member reference.
class BatchTextVisitor final : public nrbf::Visitor {
public:
    std::string result;

    bool enter_instance(int32_t, const nrbf::ClassDef& d) override {
        // Track the full class stack. a single in_context flag was
        // wrong because nested enter_instance calls (e.g. for
        // ConnectionParameters embedded in QueryAnalyzerContext) would
        // unset it before traceRowText on the outer class was seen.
        stack_.push_back(d.name.find("QueryAnalyzerContext")
                             != std::string::npos);
        return true;
    }
    void exit_instance(int32_t, const nrbf::ClassDef&) override {
        if (!stack_.empty()) stack_.pop_back();
    }
    void member(const nrbf::ClassMember& m, const nrbf::Value& v) override {
        if (stack_.empty() || !stack_.back()) return;
        if (m.name != "traceRowText") return;
        if (v.kind == nrbf::Value::Kind::String) {
            if (!v.s.empty()) result = v.s;
        } else if (v.kind == nrbf::Value::Kind::ObjectRef && v.object_id) {
            auto it = strings_.find(v.object_id);
            if (it != strings_.end()) result = it->second;
            else pending_id_ = v.object_id;   // resolve on string_object
        }
    }
    void string_object(int32_t id, std::string_view s) override {
        if (id == pending_id_) { result.assign(s); pending_id_ = 0; }
        strings_.emplace(id, std::string(s));
    }
    bool enter_object_array(int32_t, int32_t) override { return true; }
    bool enter_primitive_array(int32_t, nrbf::PrimitiveType, int32_t) override {
        return false;
    }
private:
    // One bool per class-stack frame: true iff that frame's class is
    // QueryAnalyzerContext. Members fire against the top frame.
    std::vector<bool> stack_;
    int32_t pending_id_ = 0;
    std::unordered_map<int32_t, std::string> strings_;
};

}  // namespace

namespace {

// Pulls _ServerName, _DatabaseName, _AuthenticationType, _Login,
// _UseIntegratedSecurity, _Version out of the
// Intercerve.SqlServer.ConnectionParameters block. Same forward-
// reference handling as BatchTextVisitor: a string member's value
// may stream as an ObjectRef whose StringObject record arrives later.
class ConnectionParamsVisitor final : public nrbf::Visitor {
public:
    PeSessionConnection result;

    bool enter_instance(int32_t, const nrbf::ClassDef& d) override {
        stack_.push_back(d.name.find("ConnectionParameters")
                             != std::string::npos);
        return true;
    }
    void exit_instance(int32_t, const nrbf::ClassDef&) override {
        if (!stack_.empty()) stack_.pop_back();
    }
    void member(const nrbf::ClassMember& m, const nrbf::Value& v) override {
        if (stack_.empty() || !stack_.back()) return;
        // Strings. resolve immediately when inline, otherwise queue.
        auto stash = [&](std::string& dst) {
            if (v.kind == nrbf::Value::Kind::String) {
                dst = v.s;
            } else if (v.kind == nrbf::Value::Kind::ObjectRef && v.object_id) {
                auto it = strings_.find(v.object_id);
                if (it != strings_.end()) dst = it->second;
                else pending_.emplace_back(v.object_id, &dst);
            }
        };
        if      (m.name == "_ServerName")        stash(result.server_name);
        else if (m.name == "_DatabaseName")      stash(result.database_name);
        else if (m.name == "_AuthenticationType") stash(result.auth_type);
        else if (m.name == "_Login")             stash(result.login);
        else if (m.name == "_Version")           stash(result.server_version);
        else if (m.name == "_UseIntegratedSecurity") {
            if (v.kind == nrbf::Value::Kind::Bool) {
                result.use_integrated_security = (v.i != 0);
            }
        }
    }
    void string_object(int32_t id, std::string_view s) override {
        strings_.emplace(id, std::string(s));
        for (auto it = pending_.begin(); it != pending_.end(); ) {
            if (it->first == id) {
                it->second->assign(s);
                it = pending_.erase(it);
            } else {
                ++it;
            }
        }
    }
    bool enter_object_array(int32_t, int32_t) override { return true; }
    bool enter_primitive_array(int32_t, nrbf::PrimitiveType, int32_t) override {
        return false;
    }
private:
    std::vector<bool> stack_;
    std::unordered_map<int32_t, std::string> strings_;
    std::vector<std::pair<int32_t, std::string*>> pending_;
};

}  // namespace

PeSessionConnection read_pesession_connection_params(
        const std::string& path, int file_number) {
    PeSessionConnection out;
    ZipHandle zh(path);
    if (!zh.open) return out;
    char fname[64];
    std::snprintf(fname, sizeof(fname), "%d.queryanalysis", file_number);
    std::string blob;
    try { blob = extract_entry(zh.z, fname); }
    catch (const PeSessionError&) { return out; }
    if (blob.empty()) return out;
    ConnectionParamsVisitor v;
    try { nrbf::parse(blob, v); }
    catch (const nrbf::ParseError&) { /* return whatever we got */ }
    return std::move(v.result);
}

std::string read_pesession_batch_text(const std::string& path,
                                      int file_number) {
    ZipHandle zh(path);
    if (!zh.open) return {};
    char fname[64];
    std::snprintf(fname, sizeof(fname), "%d.queryanalysis", file_number);
    std::string blob;
    try { blob = extract_entry(zh.z, fname); }
    catch (const PeSessionError&) { return {}; }
    if (blob.empty()) return {};
    BatchTextVisitor v;
    try { nrbf::parse(blob, v); }
    catch (const nrbf::ParseError&) { return std::move(v.result); }
    return std::move(v.result);
}

namespace {

// Picks the QueryAnalyzerInput.IndexAnalyzerResultsGz byte-array out of
// the NRBF stream. The member's value is an ObjectRef to a primitive
// byte array; we record the target id at member() time and capture the
// bytes when the matching array record fires later.
class IndexAnalyzerBlobVisitor final : public nrbf::Visitor {
public:
    std::string blob;

    bool enter_instance(int32_t, const nrbf::ClassDef& d) override {
        stack_.push_back(d.name.find("QueryAnalyzerInput")
                              != std::string::npos);
        return true;
    }
    void exit_instance(int32_t, const nrbf::ClassDef&) override {
        if (!stack_.empty()) stack_.pop_back();
    }
    void member(const nrbf::ClassMember& m, const nrbf::Value& v) override {
        if (!stack_.empty() && stack_.back() &&
            m.name.find("IndexAnalyzerResultsGz") != std::string::npos &&
            v.kind == nrbf::Value::Kind::ObjectRef && v.object_id) {
            target_id_ = v.object_id;
        }
    }
    bool enter_primitive_array(int32_t id, nrbf::PrimitiveType pt,
                               int32_t len) override {
        if (id == target_id_ && pt == nrbf::PrimitiveType::Byte) {
            collecting_ = true;
            if (len > 0) blob.reserve(static_cast<size_t>(len));
        }
        return collecting_;
    }
    void primitive_array_value(int32_t, const nrbf::Value& v) override {
        if (collecting_) blob.push_back(static_cast<char>(v.u));
    }
    void exit_primitive_array(int32_t) override { collecting_ = false; }
    bool enter_object_array(int32_t, int32_t) override { return true; }
private:
    std::vector<bool> stack_;
    int32_t target_id_ = 0;
    bool collecting_ = false;
};

}  // namespace

std::string read_pesession_index_analyzer_gz(const std::string& path,
                                              int file_number) {
    ZipHandle zh(path);
    if (!zh.open) return {};
    char fname[64];
    std::snprintf(fname, sizeof(fname), "%d.queryanalysis", file_number);
    std::string blob;
    try { blob = extract_entry(zh.z, fname); }
    catch (const PeSessionError&) { return {}; }
    if (blob.empty()) return {};
    IndexAnalyzerBlobVisitor v;
    try { nrbf::parse(blob, v); }
    catch (const nrbf::ParseError&) { return std::move(v.blob); }
    return std::move(v.blob);
}

std::string read_pesession_queryanalysis_blob(const std::string& path,
                                               int file_number) {
    ZipHandle zh(path);
    if (!zh.open) return {};
    char fname[64];
    std::snprintf(fname, sizeof(fname), "%d.queryanalysis", file_number);
    try { return extract_entry(zh.z, fname); }
    catch (const PeSessionError&) { return {}; }
}

std::vector<std::string> scan_showplan_blocks(std::string_view blob) {
    std::vector<std::string> out;
    auto views = extract_showplan_blocks(blob);
    out.reserve(views.size());
    for (auto& v : views) out.emplace_back(v);
    return out;
}

PeSessionItemPayload parse_pesession_queryanalysis(std::string_view blob) {
    // One parse, four visitors: replaces four separate extract+parse passes.
    PeSessionItemPayload out;
    TraceVisitor             trace;
    ConnectionParamsVisitor  conn;
    BatchTextVisitor         batch;
    IndexAnalyzerBlobVisitor index;
    nrbf::MultiVisitor multi;
    multi.add(trace);
    multi.add(conn);
    multi.add(batch);
    multi.add(index);
    try {
        nrbf::parse(blob, multi);
    } catch (const nrbf::ParseError&) {
        // Per-visitor contract: swallow parse errors, keep what was captured.
    }
    out.traces            = std::move(trace.out);
    out.connection_params = std::move(conn.result);
    out.batch_text        = std::move(batch.result);
    out.index_analyzer_gz = std::move(index.blob);
    return out;
}

PeSessionItemPayload read_pesession_item_payload(const std::string& path,
                                                  int file_number) {
    std::string blob = read_pesession_queryanalysis_blob(path, file_number);
    if (blob.empty()) return {};
    PeSessionItemPayload out = parse_pesession_queryanalysis(blob);
    out.showplan_xml_blocks = scan_showplan_blocks(blob);
    return out;
}

}  // namespace pesession
