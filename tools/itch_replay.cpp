// fpga-sim-itch-replay: replay a bounded slice of a REAL NASDAQ TotalView-ITCH 5.0 file
// through the parser, order book and cycle model, and report what is in it.
//
//   fpga-sim-itch-replay <decompressed-itch-file> [--symbol XYZ] [--model-messages N]
//                                                 [--gap-cap-cycles C]
//
// Everything printed about latency is SIMULATED CYCLES of this model driven by real
// messages. It is not a hardware measurement and not a statement about any real FPGA.
#include "core/discrete_engine.hpp"
#include "pipeline/real_itch.hpp"
#include "pipeline/tick_to_trade.hpp"
#include "itch_analysis.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {

using namespace itch_tools;
using namespace fpga_sim;

constexpr std::size_t kModelCapacity = 20000;
constexpr std::size_t kOrderCapacity = 16384;
using Stream = ReplayStream<kModelCapacity, kOrderCapacity>;
using Pipeline = BasicTickToTradePipeline<Stream>;

std::string stock_of(const std::uint8_t* r) {   // 'R' message: stock at offset 11, 8 bytes space padded
    std::string s(reinterpret_cast<const char*>(r + 11), 8);
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

bool book_type(std::uint8_t t) { return t == 'A' || t == 'F' || t == 'E' || t == 'C' || t == 'X' || t == 'D' || t == 'U'; }

void print_series(const char* label, std::vector<std::uint64_t> v) {
    std::printf("  %-22s min %llu  median %llu  p99 %llu  max %llu\n", label,
                static_cast<unsigned long long>(*std::min_element(v.begin(), v.end())),
                static_cast<unsigned long long>(percentile(v, 50)), static_cast<unsigned long long>(percentile(v, 99)),
                static_cast<unsigned long long>(*std::max_element(v.begin(), v.end())));
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: %s <decompressed-itch-file> [--symbol XYZ] [--model-messages N] [--gap-cap-cycles C]\n", argv[0]); return 2; }
    std::string want_symbol;
    std::size_t model_messages = kModelCapacity;
    std::uint64_t gap_cap = 256;
    for (int i = 2; i + 1 < argc; i += 2) {
        if (!std::strcmp(argv[i], "--symbol")) want_symbol = argv[i + 1];
        else if (!std::strcmp(argv[i], "--model-messages")) model_messages = std::min<std::size_t>(std::strtoull(argv[i + 1], nullptr, 10), kModelCapacity);
        else if (!std::strcmp(argv[i], "--gap-cap-cycles")) gap_cap = std::strtoull(argv[i + 1], nullptr, 10);
        else { std::fprintf(stderr, "unknown option %s\n", argv[i]); return 2; }
    }
    const int fd = open(argv[1], O_RDONLY);
    struct stat st{};
    if (fd < 0 || fstat(fd, &st) != 0) { std::perror("open"); return 2; }
    const auto size = static_cast<std::size_t>(st.st_size);
    const auto* data = static_cast<const std::uint8_t*>(mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0));
    if (data == MAP_FAILED) { std::perror("mmap"); return 2; }

    // ---------------- pass 1: message mix, directory, activity per stock ----------------
    std::map<std::uint8_t, std::uint64_t> mix;
    std::map<std::uint16_t, std::string> directory;
    std::map<std::uint16_t, std::uint64_t> adds_by_locate;
    std::uint64_t total = 0, first_ts = 0, last_ts = 0;
    std::map<std::uint64_t, std::uint32_t> per_ms;   // messages per millisecond bucket
    Walker w(data, size);
    std::size_t len = 0;
    while (const std::uint8_t* m = w.next(len)) {
        ++total; ++mix[m[0]];
        const std::uint64_t ts = timestamp_ns(m);
        if (first_ts == 0) first_ts = ts;
        last_ts = ts;
        ++per_ms[ts / 1000000ULL];
        if (m[0] == 'R') directory[locate_of(m)] = stock_of(m);
        if (m[0] == 'A' || m[0] == 'F') ++adds_by_locate[locate_of(m)];
    }
    std::printf("fpga-sim-itch-replay: REAL NASDAQ TotalView-ITCH 5.0 messages (not synthetic)\n");
    std::printf("input: %zu bytes of decompressed ITCH; %llu messages parsed; framing stop: %s\n", size,
                static_cast<unsigned long long>(total),
                w.stop == Walker::Stop::end ? "clean end" : w.stop == Walker::Stop::truncated_tail ? "truncated final message (slice boundary)" : "BAD FRAMING");
    if (w.stop == Walker::Stop::bad_framing) { std::fprintf(stderr, "framing error at byte %zu\n", w.offset); return 1; }
    std::printf("message timestamps in slice: %.3f s of exchange time since midnight (first %.3f s, last %.3f s)\n",
                (last_ts - first_ts) / 1e9, first_ts / 1e9, last_ts / 1e9);
    std::printf("\nmessage mix (all symbols):\n");
    std::vector<std::pair<std::uint8_t, std::uint64_t>> mixv(mix.begin(), mix.end());
    std::sort(mixv.begin(), mixv.end(), [](auto& a, auto& b) { return a.second > b.second; });
    for (auto& [t, n] : mixv) std::printf("  %c  %-28s %12llu  %6.2f%%\n", t, type_name(t), static_cast<unsigned long long>(n), 100.0 * static_cast<double>(n) / static_cast<double>(total));
    std::uint64_t peak_ms = 0;
    for (auto& [ms, n] : per_ms) peak_ms = std::max<std::uint64_t>(peak_ms, n);
    std::printf("busiest 1 ms of exchange time: %llu messages across all symbols\n", static_cast<unsigned long long>(peak_ms));

    // ---------------- choose the stock the single-symbol model will follow ----------------
    std::uint16_t locate = 0;
    if (!want_symbol.empty()) {
        for (auto& [l, s] : directory) if (s == want_symbol) locate = l;
        if (!locate) { std::fprintf(stderr, "symbol %s not in the slice's stock directory\n", want_symbol.c_str()); return 1; }
    } else {
        std::uint64_t best = 0;
        for (auto& [l, n] : adds_by_locate) if (n > best) { best = n; locate = l; }
    }
    const std::string symbol = directory.count(locate) ? directory[locate] : "?";
    std::printf("\nstock followed by the single-symbol book and model: %s (stock locate %u; %llu add orders in the slice)\n",
                symbol.c_str(), locate, static_cast<unsigned long long>(adds_by_locate[locate]));

    // ---------------- pass 2: full-depth real book + model replay ----------------
    static Stream stream;
    RealBook book;
    std::vector<std::uint64_t> bid_levels, ask_levels, orders;
    std::uint64_t book_msgs = 0, over5 = 0;
    std::uint64_t prev_ts = 0, arrival = 0;
    bool have_prev = false;
    Walker w2(data, size);
    while (const std::uint8_t* m = w2.next(len)) {
        if (!book_type(m[0]) || locate_of(m) != locate) continue;
        book.apply(m);
        ++book_msgs;
        bid_levels.push_back(book.bid_levels());
        ask_levels.push_back(book.ask_levels());
        orders.push_back(book.orders());
        if (book.bid_levels() > FiveLevelOrderBook::levels || book.ask_levels() > FiveLevelOrderBook::levels) ++over5;
        if (stream.count() < model_messages) {
            const std::uint64_t ts = timestamp_ns(m);
            if (have_prev) {
                const std::uint64_t dt_ns = ts > prev_ts ? ts - prev_ts : 0;
                const std::uint64_t dt_cycles = dt_ns * 165ULL / 512ULL;   // exact: 1 cycle = 512/165 ns
                arrival += std::min(dt_cycles, gap_cap);
            }
            prev_ts = ts; have_prev = true;
            stream.push(m, len, arrival);
        }
    }
    if (book_msgs == 0) { std::fprintf(stderr, "no book messages for that stock in the slice\n"); return 1; }
    std::printf("\nreal order book for %s over the slice (%llu book-changing messages, full depth, rebuilt from the messages):\n", symbol.c_str(), static_cast<unsigned long long>(book_msgs));
    print_series("bid price levels", bid_levels);
    print_series("ask price levels", ask_levels);
    print_series("resting orders", orders);
    std::printf("  after %.2f%% of messages either side held more than the model's %zu levels\n", 100.0 * static_cast<double>(over5) / static_cast<double>(book_msgs), FiveLevelOrderBook::levels);
    std::printf("  messages referencing an order not seen in the slice: %llu\n", static_cast<unsigned long long>(book.unknown_refs()));

    static Pipeline pipeline(stream);
    DiscreteEngine engine;
    if (!engine.on_posedge(&Pipeline::posedge_callback, &pipeline) || !engine.on_negedge(&Pipeline::negedge_callback, &pipeline)) return 1;
    const std::uint64_t limit = (stream.message(stream.count() - 1).arrival_cycle) + 100000ULL;
    while (!pipeline.done() && engine.cycle() < limit) engine.step();
    std::printf("\ncycle model on the first %zu messages of %s (arrival times from the real timestamps; idle gaps capped at %llu cycles):\n",
                stream.count(), symbol.c_str(), static_cast<unsigned long long>(gap_cap));
    if (!pipeline.ok()) {
        std::printf("  model stopped after %zu of %zu messages (a stage reported an error, e.g. a reference it could not resolve)\n", pipeline.completed(), stream.count());
        return 1;
    }
    static std::array<std::uint32_t, kModelCapacity> scratch;
    const LatencyStats s = pipeline.stats_with(scratch.data());
    std::map<char, std::size_t> model_mix;
    for (std::size_t i = 0; i < stream.count(); ++i) ++model_mix[stream.message(i).type];
    std::printf("  messages replayed:");
    for (auto& [t, n] : model_mix) std::printf(" %c=%zu", t, n);
    std::printf("\n  simulated cycles run: %llu\n", static_cast<unsigned long long>(engine.cycle()));
    std::printf("tick-to-trade latency, SIMULATED CYCLES on real messages (not a hardware measurement):\n");
    const auto line = [](const char* label, std::uint32_t c) {
        const std::uint64_t e4 = DiscreteEngine::cycles_to_ns_e4(c);
        std::printf("  %-7s %4u cycles = %llu.%04llu ns\n", label, c, static_cast<unsigned long long>(e4 / 10000ULL), static_cast<unsigned long long>(e4 % 10000ULL));
    };
    line("min", s.min); line("median", s.median); line("p99", s.p99); line("max", s.max);
    std::printf("histogram (2-cycle bins, last bin includes overflow):\n");
    for (std::size_t b = 0; b < LatencyStats::histogram_bins; ++b)
        if (s.histogram[b]) std::printf("  %3zu-%3zu cycles %6u\n", b * LatencyStats::bin_width_cycles, b * LatencyStats::bin_width_cycles + 1, s.histogram[b]);
    std::printf("model order book (%zu levels) adds dropped because a side was full: %llu\n", FiveLevelOrderBook::levels, static_cast<unsigned long long>(pipeline.book_overflow_count()));
    return 0;
}
