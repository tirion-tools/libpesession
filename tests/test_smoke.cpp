// Headers-compile + types-default-construct smoke test.
// End-to-end coverage (open + parse + lossless verify) lives in
// the Calliper tool's verify_lossless binary.

#include "pesession/pesession.hpp"
#include "pesession/pesession_to_osession.hpp"

#include <cstdio>

int main() {
    pesession::PeSessionMeta m{};
    pesession::PeSessionConnection c{};
    pesession::StatementRuntimeAgg s{};
    pesession::TraceMetrics t{};
    pesession::WaitEntry w{};
    pesession::SessionTraceData d{};
    pesession::ConvertReport r{};

    (void)m; (void)c; (void)s; (void)t;
    (void)w; (void)d; (void)r;

    std::printf("libpesession smoke test PASS\n");
    return 0;
}
