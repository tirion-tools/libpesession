#include "pesession/pesession_to_osession.hpp"

#include "pesession/pesession.hpp"
#include <osession/osession.hpp>
#include <showplan/showplan.hpp>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace pesession {

namespace {

// ── XML normalization for plan-shape dedup ──────────────────────────────────
// The osession `plans` table is a dedup index keyed by the hash of a
// "shape-only" version of the XML. Per-snapshot runtime data
// (RunTimeInformation / QueryTimeStats / MemoryGrantInfo / WaitStats)
// and per-call attribute values (StatementId, ParameterRuntimeValue,
// compile-timing noise) all get stripped or blanked before hashing.
// The original XML is preserved verbatim in plan_snapshots. see the
// lossless verifier.

std::string strip_element(std::string_view xml, std::string_view tag) {
    std::string out;
    out.reserve(xml.size());
    const std::string open_prefix = "<" + std::string(tag);
    const std::string close       = "</" + std::string(tag) + ">";
    size_t pos = 0;
    while (pos < xml.size()) {
        size_t p = xml.find(open_prefix, pos);
        if (p == std::string_view::npos) {
            out.append(xml.data() + pos, xml.size() - pos);
            break;
        }
        char next = (p + open_prefix.size() < xml.size())
                        ? xml[p + open_prefix.size()] : '\0';
        if (next != ' ' && next != '/' && next != '>'
            && next != '\t' && next != '\r' && next != '\n') {
            out.append(xml.data() + pos, p + open_prefix.size() - pos);
            pos = p + open_prefix.size();
            continue;
        }
        out.append(xml.data() + pos, p - pos);
        size_t end = xml.find('>', p + open_prefix.size());
        if (end == std::string_view::npos) {
            out.append(xml.data() + p, xml.size() - p);
            break;
        }
        if (xml[end - 1] == '/') {
            pos = end + 1;
            continue;
        }
        size_t close_pos = xml.find(close, end + 1);
        if (close_pos == std::string_view::npos) {
            out.append(xml.data() + p, xml.size() - p);
            break;
        }
        pos = close_pos + close.size();
    }
    return out;
}

std::string blank_attr(std::string_view xml, std::string_view name) {
    std::string out;
    out.reserve(xml.size());
    const std::string needle = std::string(name) + "=\"";
    size_t pos = 0;
    while (pos < xml.size()) {
        size_t p = xml.find(needle, pos);
        if (p == std::string_view::npos) {
            out.append(xml.data() + pos, xml.size() - pos);
            break;
        }
        char prev = (p > 0) ? xml[p - 1] : '\0';
        if (prev != ' ' && prev != '\t' && prev != '\r' && prev != '\n'
            && prev != '<') {
            out.append(xml.data() + pos, p + needle.size() - pos);
            pos = p + needle.size();
            continue;
        }
        out.append(xml.data() + pos, p - pos);
        out.append(needle);
        size_t qpos = xml.find('"', p + needle.size());
        if (qpos == std::string_view::npos) {
            out.append(xml.data() + p + needle.size(),
                       xml.size() - p - needle.size());
            break;
        }
        out.append("\"");
        pos = qpos + 1;
    }
    return out;
}

// Trims trailing ASCII whitespace. TraceRowEx.TextData and
// StmtSimple/@StatementText come from the same SQL source but
// SQL Server's trace stream sometimes carries one extra trailing
// tab/CR/LF that the showplan stripper drops. a byte-exact match
// fails on that alone.
std::string rtrim_copy(const std::string& s) {
    size_t e = s.size();
    while (e > 0 && (s[e-1] == ' '  || s[e-1] == '\t' ||
                     s[e-1] == '\r' || s[e-1] == '\n')) {
        --e;
    }
    return s.substr(0, e);
}

std::string normalize_plan_xml(std::string_view xml) {
    std::string s = strip_element(xml, "RunTimeInformation");
    s = strip_element(s, "QueryTimeStats");
    s = strip_element(s, "WaitStats");
    s = strip_element(s, "MemoryGrantInfo");
    s = blank_attr(s, "StatementId");
    s = blank_attr(s, "StatementCompId");
    s = blank_attr(s, "RetrievedFromCache");
    s = blank_attr(s, "ParameterRuntimeValue");
    s = blank_attr(s, "CompileTime");
    s = blank_attr(s, "CompileCPU");
    return s;
}

}  // namespace

ConvertReport convert_pesession_to_osession(const std::string& in_path,
                                            const std::string& out_path) {
    ConvertReport rep;
    namespace fs = std::filesystem;
    if (!fs::exists(in_path)) {
        rep.error = "input does not exist: " + in_path;
        return rep;
    }
    std::error_code ec;
    fs::remove(out_path, ec);  // start clean

    auto t0 = std::chrono::steady_clock::now();

    std::vector<PeSessionMeta> idx;
    try {
        idx = read_pesession_index(in_path);
    } catch (const std::exception& e) {
        rep.error = std::string("index error: ") + e.what();
        return rep;
    }
    rep.items = static_cast<int>(idx.size());

    try {
        osession::File out(out_path, osession::File::Mode::Write);
        out.set_meta("source_pesession", in_path);
        out.begin_transaction();

        for (auto& meta : idx) {
            osession::ItemMeta im;
            im.file_number = meta.file_number;
            im.instance    = meta.instance;
            im.database    = meta.database;
            im.login       = meta.login;
            im.created_utc = meta.created_utc;
            im.total_time  = meta.total_time;
            im.actual_rows = meta.actual_rows;
            im.plan_type   = meta.plan_type;
            // Pull the user-submitted batch SQL (the QueryAnalyzer
            // context's traceRowText) and persist it alongside the
            // item so the openplan reader can show it without re-
            // parsing the NRBF.
            try {
                im.batch_text = read_pesession_batch_text(
                    in_path, meta.file_number);
            } catch (...) {}
            try {
                // Stored verbatim. gzipped JSON. The Index Analysis
                // panel decompresses + parses on demand.
                im.index_analyzer_gz = read_pesession_index_analyzer_gz(
                    in_path, meta.file_number);
            } catch (...) {}
            // ConnectionParameters. auth type + server version round-
            // trip so openplan can show the same identity SQL Sentry
            // would. Login from ConnectionParameters wins over the
            // PeSessionMeta login only when the meta value is empty
            // (the JSON manifest sometimes omits it).
            try {
                auto cp = read_pesession_connection_params(
                    in_path, meta.file_number);
                im.auth_type      = cp.auth_type;
                im.server_version = cp.server_version;
                if (im.login.empty()) im.login = cp.login;
            } catch (...) {}
            out.add_item(im);

            std::vector<std::string> blocks;
            try {
                blocks = extract_pesession_xml_blocks(in_path,
                                                      meta.file_number);
            } catch (const std::exception&) {
                continue;
            }

            // Read trace events once. Each TraceRowEx is one
            // sp_statement_completed firing. including EXEC
            // dispatcher statements that have no ShowPlanXML of
            // their own. We emit one statements-row per trace event
            // so the grid matches what SQL Sentry shows.
            SessionTraceData td;
            try { td = read_pesession_traces(in_path, meta.file_number); }
            catch (const std::exception&) {}

            // Walk plan blocks: persist plans + snapshots, then
            // index each StmtSimple by (parent_object_id,
            // statement_text) so traces below can attach a plan_id.
            // Pesession traces emitted by SQL Sentry's older trace
            // engine carry no PlanHandle on the trace row, so text-
            // match is the only join key that survives.
            struct PlanRef {
                int64_t plan_id = 0;
                int64_t parent_object_id = 0;
            };
            std::unordered_map<std::string, PlanRef> text_to_plan;
            int snapshot_idx = 0;
            for (auto& xml : blocks) {
                std::string normalized = normalize_plan_xml(xml);
                int64_t plan_id = 0;
                try { plan_id = out.add_plan(normalized); }
                catch (const std::exception&) { continue; }
                ++rep.blocks;

                try {
                    osession::PlanSnapshot snap;
                    snap.item         = meta.file_number;
                    snap.snapshot_idx = snapshot_idx++;
                    snap.plan_id      = plan_id;
                    out.add_plan_snapshot(snap, xml);
                } catch (const std::exception&) {}

                try {
                    auto plan = showplan::parse_xml(xml);
                    for (auto& s : plan.statements) {
                        if (s.text.empty()) continue;
                        PlanRef r;
                        r.plan_id = plan_id;
                        r.parent_object_id = s.parent_object_id;
                        text_to_plan[rtrim_copy(s.text)] = r;
                    }
                } catch (const showplan::ParseError&) {}
            }

            // Statements: one row per TraceRowEx. statement_id
            // matches the trace's document-order index so the
            // trace_events loop below can FK directly without a
            // separate join table.
            int statements_written = 0;
            if (!td.traces.empty()) {
                for (size_t i = 0; i < td.traces.size(); ++i) {
                    const auto& m = td.traces[i];
                    osession::Statement st;
                    st.item             = meta.file_number;
                    st.statement_id     = static_cast<int>(i);
                    st.text             = m.text;
                    st.object_name      = m.object_name;
                    st.nest_level       = m.nest_level;
                    st.line_number      = m.line_number;
                    st.source_offset    = m.offset_bytes;
                    auto pit = text_to_plan.find(rtrim_copy(m.text));
                    if (pit != text_to_plan.end()) {
                        st.plan_id          = pit->second.plan_id;
                        st.parent_object_id = pit->second.parent_object_id;
                    }
                    out.add_statement(st);
                    ++rep.statements;
                    ++statements_written;
                }
            } else {
                // Estimated-plan pesessions have no trace events;
                // fall back to one statements-row per parsed
                // StmtSimple so the grid still shows something.
                // Plans were already persisted in the walk above;
                // re-parse to recover statement ordering and look
                // up the existing plan_id via text_to_plan.
                int stmt_idx = 0;
                for (auto& xml : blocks) {
                    try {
                        auto plan = showplan::parse_xml(xml);
                        for (auto& s : plan.statements) {
                            osession::Statement st;
                            st.item             = meta.file_number;
                            st.statement_id     = stmt_idx++;
                            st.text             = s.text;
                            auto pit = text_to_plan.find(rtrim_copy(s.text));
                            if (pit != text_to_plan.end()) {
                                st.plan_id = pit->second.plan_id;
                            }
                            st.parent_object_id = s.parent_object_id;
                            out.add_statement(st);
                            ++rep.statements;
                            ++statements_written;
                        }
                    } catch (...) {}
                }
            }

            try {
                auto rt = read_pesession_runtime(in_path, meta.file_number);
                for (size_t i = 0;
                     i < rt.size() &&
                     i < static_cast<size_t>(statements_written);
                     ++i) {
                    osession::RuntimeSample r;
                    r.item         = meta.file_number;
                    r.statement_id = static_cast<int>(i);
                    r.cpu_us       = rt[i].cpu_ms * 1000;
                    r.elapsed_us   = rt[i].elapsed_ms * 1000;
                    r.reads        = rt[i].logical_reads + rt[i].physical_reads;
                    r.writes       = rt[i].write_pages;
                    r.row_count    = rt[i].row_count;
                    out.add_runtime(r);
                    ++rep.runtime_rows;
                }
            } catch (const std::exception&) {}

            // Trace events: statement_id == trace position by
            // construction above. Wait aggregates still need
            // plan_handle → statement_id lookup since waits live
            // inside QueryStats/PlanData scopes rather than under a
            // specific TraceRowEx. fall back to plan_handles[]
            // positional indexing (which is what the original code
            // did, just rewritten here for clarity).
            std::unordered_map<std::string, int> ph_to_idx;
            for (size_t i = 0; i < td.plan_handles.size(); ++i) {
                if (!td.plan_handles[i].empty()) {
                    ph_to_idx.emplace(td.plan_handles[i],
                                      static_cast<int>(i));
                }
            }
            try {
                std::unordered_map<int, std::vector<osession::TraceEvent>>
                    bucket;
                for (size_t i = 0; i < td.traces.size(); ++i) {
                    const auto& m = td.traces[i];
                    osession::TraceEvent ev;
                    ev.item           = meta.file_number;
                    ev.statement_id   = static_cast<int>(i);
                    ev.trace_pos      = static_cast<int32_t>(i);
                    ev.cpu_us         = m.cpu_us;
                    ev.elapsed_us     = m.duration_us;
                    ev.udf_cpu_us     = m.udf_cpu_us;
                    ev.udf_elapsed_us = m.udf_duration_us;
                    ev.reads          = m.reads;
                    ev.writes         = m.writes;
                    ev.row_count      = m.row_count;
                    ev.start_dt_raw   = m.start_dt_raw;
                    ev.end_dt_raw     = m.end_dt_raw;
                    ev.object_id      = m.object_id;
                    ev.nest_level     = m.nest_level;
                    ev.line_number    = m.line_number;
                    ev.offset_bytes   = m.offset_bytes;
                    if (m.object_id != 0 && !m.object_name.empty()) {
                        out.add_object_name(m.object_id, m.object_name);
                    }
                    bucket[ev.statement_id].push_back(std::move(ev));
                    ++rep.trace_events;
                }
                for (auto& kv : bucket) {
                    out.add_trace_events(meta.file_number, kv.first,
                                         kv.second);
                }
                for (const auto& w : td.waits) {
                    int sid = -1;
                    if (!w.parent_plan_handle_hex.empty()) {
                        auto it = ph_to_idx.find(w.parent_plan_handle_hex);
                        if (it != ph_to_idx.end()) sid = it->second;
                    }
                    osession::WaitAggregate ow;
                    ow.item         = meta.file_number;
                    ow.statement_id = sid;
                    ow.wait_type    = w.wait_type;
                    ow.duration_ms  = w.total_duration_ms;
                    ow.signal_ms    = w.total_signal_ms;
                    out.add_wait(ow);
                    ++rep.waits;
                }
            } catch (const std::exception&) {}
        }
        out.commit();
        // Bracket-close the File so the WAL is checkpointed back into
        // the main file before we measure on-disk size below.
    } catch (const std::exception& e) {
        rep.error = e.what();
        return rep;
    }

    rep.in_size  = static_cast<int64_t>(fs::file_size(in_path, ec));
    rep.out_size = static_cast<int64_t>(fs::file_size(out_path, ec));
    rep.elapsed_s = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - t0).count();
    return rep;
}

}  // namespace pesession
