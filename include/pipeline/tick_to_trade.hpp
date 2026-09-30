#pragma once

// Tick-to-trade pipeline MODEL driven by the discrete engine's cycle counter.
//
// What is measured: for each message of a SYNTHETIC ITCH stream, the number of
// simulated clock cycles from the cycle its first PCS block reaches the
// receiver (scheduled arrival, so queueing behind earlier messages on the wire
// is included) to the cycle a decision record is posted to the DMA ring
// (inclusive count). This is a property of this model, NOT a hardware
// measurement and NOT a claim about any real FPGA.
//
// Where the cycles come from (all stages run the real module code):
//   PCS      1 block/cycle. Block count = ceil((len+4)/7) from the actual
//            Deserializer66b::encode output; CRC checked by decode(). [data]
//   MAC      1 64-byte AXI4-Stream beat/cycle, beats = ceil(len/64), each
//            passed to MacFramer::accept(). IPG assumed 12 idle bytes. [data]
//   PARSE    Itch50Parser::parse() has no timing of its own. FIXED modelled
//            latency parser_cycles = 2 (documented parameter, not derived).
//   BOOK     ItchBookApplier (OrderTable + FiveLevelOrderBook) has no
//            timing of its own. FIXED modelled latency book_cycles = 1
//            (documented parameter, not derived).
//   QUOTE RAM best quote written to DualPortBram<BookQuote, 32, PipelineParams::bram_latency>
//            and read back; latency comes from the BRAM model's template
//            parameter as it executes. [module]
//   OFI      OfiDsp::pipeline_stages = 3 stages as it executes. [module]
//   DMA      PcieGen4x16Dma has no timing of its own. FIXED modelled latency
//            dma_post_cycles = 2 (TLP build + post, documented parameter).
//            The host drain (consume + MSI-X clear) is instantaneous.
// Every stage hand-off is a one-cycle register. Stages hold one message and
// back-pressure the stage before them; there is no other queueing except the
// unbounded-by-design wire (arrival schedule) and a 64-entry queue in front of
// the DMA stage.
//
// Determinism: integer-only decision paths, fixed-size std::array storage, no
// heap, no unordered containers, no wall-clock, all state value-initialised.

#include "bus/axi4_stream.hpp"
#include "core/aligned_slab.hpp"
#include "core/discrete_engine.hpp"
#include "modules/bram_memory.hpp"
#include "modules/mac_framer.hpp"
#include "modules/ofi_dsp.hpp"
#include "modules/itch_book.hpp"
#include "modules/parser_itch50.hpp"
#include "modules/pcie_dma.hpp"
#include "modules/phy_pcs.hpp"
#include "pipeline/synthetic_itch.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace fpga_sim {

struct PipelineParams {
    static constexpr std::uint32_t parser_cycles = 2;     // fixed, documented (not derived)
    static constexpr std::uint32_t book_cycles = 1;       // fixed, documented (not derived)
    static constexpr std::uint32_t dma_post_cycles = 2;   // fixed, documented (not derived)
    static constexpr std::size_t mac_ipg_idle_bytes = 12;
    static constexpr std::size_t bram_latency = 1;        // DualPortBram Latency template argument
};

struct LatencyStats {
    static constexpr std::size_t histogram_bins = 32;
    static constexpr std::uint32_t bin_width_cycles = 2;   // last bin also collects overflow

    std::size_t count = 0;
    std::uint32_t min = 0, median = 0, p99 = 0, max = 0;   // simulated cycles
    std::array<std::uint32_t, histogram_bins> histogram{};

    // Nearest-rank percentiles on the sorted samples (integer index arithmetic).
    static LatencyStats compute(const std::uint32_t* samples, std::size_t n) noexcept {
        LatencyStats s{};
        if (n == 0) return s;
        std::array<std::uint32_t, SyntheticItchStream::message_count> sorted{};
        for (std::size_t i = 0; i < n; ++i) sorted[i] = samples[i];
        std::sort(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(n));
        s.count = n;
        s.min = sorted[0];
        s.max = sorted[n - 1];
        s.median = sorted[(50 * n + 99) / 100 - 1];
        s.p99 = sorted[(99 * n + 99) / 100 - 1];
        for (std::size_t i = 0; i < n; ++i) {
            std::size_t bin = samples[i] / bin_width_cycles;
            if (bin >= histogram_bins) bin = histogram_bins - 1;
            ++s.histogram[bin];
        }
        return s;
    }
};

class TickToTradePipeline {
public:
    static constexpr std::size_t message_count = SyntheticItchStream::message_count;

    explicit TickToTradePipeline(const SyntheticItchStream& stream) noexcept : stream_(stream) {}

    static void posedge_callback(void* context, std::uint64_t cycle) noexcept {
        static_cast<TickToTradePipeline*>(context)->posedge(cycle);
    }
    static void negedge_callback(void* context, std::uint64_t cycle) noexcept {
        static_cast<TickToTradePipeline*>(context)->negedge(cycle);
    }

    [[nodiscard]] bool done() const noexcept { return completed_ == message_count || error_; }
    [[nodiscard]] bool ok() const noexcept { return !error_ && completed_ == message_count; }
    [[nodiscard]] std::size_t completed() const noexcept { return completed_; }
    [[nodiscard]] const std::uint32_t* latencies() const noexcept { return latency_.data(); }
    [[nodiscard]] std::int32_t message_ofi(std::size_t i) const noexcept { return state_[i].ofi; }
    [[nodiscard]] const BookQuote& message_quote(std::size_t i) const noexcept { return state_[i].quote; }
    [[nodiscard]] std::int64_t cumulative_ofi() const noexcept { return cumulative_ofi_; }
    [[nodiscard]] std::int32_t last_ofi() const noexcept { return last_ofi_; }
    [[nodiscard]] bool wire_busy() const noexcept { return pcs_.msg >= 0; }
    [[nodiscard]] std::uint64_t book_overflow_count() const noexcept { return applier_.book().overflow_count(); }
    [[nodiscard]] std::size_t dma_posted() const noexcept { return dma_.posted_count(); }
    [[nodiscard]] LatencyStats stats() const noexcept {
        return LatencyStats::compute(latency_.data(), completed_);
    }

private:
    struct Slot {
        std::int32_t msg = -1;
        std::uint64_t avail = 0;   // first cycle in which the stage may work on msg
        std::uint32_t left = 0;    // work cycles remaining
        [[nodiscard]] bool free() const noexcept { return msg < 0; }
        // Consumes one work cycle if the stage has the message and it is available.
        bool work(std::uint64_t cycle) noexcept {
            if (msg >= 0 && cycle >= avail && left > 0) { --left; return true; }
            return false;
        }
        [[nodiscard]] bool finished(std::uint64_t cycle) const noexcept {
            return msg >= 0 && cycle >= avail && left == 0;
        }
        void load(std::int32_t m, std::uint64_t a, std::uint32_t l) noexcept { msg = m; avail = a; left = l; }
    };

    struct MsgState {
        std::array<std::uint64_t, 8> blocks{};
        std::size_t block_count = 0;
        std::array<std::uint8_t, 48> payload{};
        std::array<std::uint8_t, 128> frame{};
        std::size_t frame_len = 0;
        ItchEvent event{};
        BookQuote quote{};
        std::int32_t ofi = 0;
    };

    void fail() noexcept { error_ = true; }

    void posedge(std::uint64_t cycle) noexcept {
        if (error_) { bram_.posedge({}, {}); ofi_.posedge_idle(); return; }
        DualPortBram<BookQuote, 32, PipelineParams::bram_latency>::PortRequest bram_a{}, bram_b{};

        // ---- DMA (downstream-first so a freed slot is visible to upstream) ----
        if (dma_.free()) {
            std::int32_t id = -1;
            if (dma_q_.pop(id)) dma_.load(id, cycle, PipelineParams::dma_post_cycles);
        }
        if (dma_.work(cycle) && dma_.left == 0) post_decision(cycle);

        // ---- OFI issue: one quote per cycle, bubble otherwise ----
        if (ofi_in_.msg >= 0 && cycle >= ofi_in_.avail) {
            ofi_.posedge(state_[static_cast<std::size_t>(ofi_in_.msg)].quote);
            if (!ofi_ring_.push(ofi_in_.msg)) fail();
            ofi_in_.msg = -1;
        } else {
            ofi_.posedge_idle();
        }

        // ---- quote RAM read-back (port B) ----
        if (rd_.msg >= 0 && cycle >= rd_.avail) {
            bram_b.read = true;
            bram_b.address = static_cast<std::size_t>(rd_.msg) % 32;
            if (!bram_ring_.push(rd_.msg)) fail();
            rd_.msg = -1;
        }

        // ---- BOOK ----
        book_slot_.work(cycle);
        if (book_slot_.finished(cycle) && rd_.free()) {
            const auto id = static_cast<std::size_t>(book_slot_.msg);
            apply_to_book(state_[id]);
            bram_a.write = true;
            bram_a.address = id % 32;
            bram_a.write_data = state_[id].quote;
            rd_.load(book_slot_.msg, cycle + 1, 1);
            book_slot_.msg = -1;
        }

        // ---- PARSE ----
        parse_.work(cycle);
        if (parse_.finished(cycle) && book_slot_.free()) {
            MsgState& s = state_[static_cast<std::size_t>(parse_.msg)];
            const std::size_t n = parser_.parse(s.frame.data(), s.frame_len);
            if (n != 1 || parser_.status() != ParseStatus::ok || parser_.bytes_consumed() != s.frame_len) fail();
            s.event = parser_.event(0);
            book_slot_.load(parse_.msg, cycle + 1, PipelineParams::book_cycles);
            parse_.msg = -1;
        }

        // ---- MAC: one AXI4-Stream beat per work cycle ----
        if (mac_.msg >= 0 && cycle >= mac_.avail && mac_.left > 0) {
            MsgState& s = state_[static_cast<std::size_t>(mac_.msg)];
            const std::size_t beat_index = mac_beats_total_ - mac_.left;
            const std::size_t offset = beat_index * 64;
            const std::size_t remaining = s.frame_len - offset;
            const std::size_t n = remaining < 64 ? remaining : 64;
            Axi4Stream512 beat{};
            std::memcpy(beat.tdata.data(), s.payload.data() + offset, n);
            beat.tkeep = n == 64 ? ~0ULL : ((1ULL << n) - 1ULL);
            beat.tvalid = beat.tready = true;
            beat.tlast = (mac_.left == 1);
            if (!mac_framer_.accept(beat)) fail();
            std::memcpy(s.frame.data() + offset, beat.tdata.data(), n);
            --mac_.left;
        }
        if (mac_.finished(cycle) && parse_.free()) {
            parse_.load(mac_.msg, cycle + 1, PipelineParams::parser_cycles);
            mac_.msg = -1;
        }

        // ---- PCS: pick the next arrived message, receive one block per cycle ----
        if (pcs_.free() && next_msg_ < message_count && cycle >= stream_.message(next_msg_).arrival_cycle) {
            const SyntheticMessage& m = stream_.message(next_msg_);
            MsgState& s = state_[next_msg_];
            s.block_count = Deserializer66b::encode(m.bytes.data(), m.length, s.blocks.data(), s.blocks.size());
            if (s.block_count == 0) fail();
            pcs_.load(static_cast<std::int32_t>(next_msg_), cycle, static_cast<std::uint32_t>(s.block_count));
            ++next_msg_;
        }
        pcs_.work(cycle);
        if (pcs_.finished(cycle) && mac_.free()) {
            const auto id = static_cast<std::size_t>(pcs_.msg);
            MsgState& s = state_[id];
            const std::size_t len = stream_.message(id).length;
            const PcsResult r = pcs_decoder_.decode(s.blocks.data(), s.block_count, s.payload.data(),
                                                    s.payload.size(), len);
            if (!r.block_lock || !r.crc_valid || r.payload_length != len) fail();
            s.frame_len = len;
            mac_beats_total_ = (len + 63) / 64;
            if (!mac_framer_.verify_ipg(PipelineParams::mac_ipg_idle_bytes)) fail();
            mac_.load(pcs_.msg, cycle + 1, static_cast<std::uint32_t>(mac_beats_total_));
            pcs_.msg = -1;
        }

        bram_.posedge(bram_a, bram_b);
    }

    void negedge(std::uint64_t cycle) noexcept {
        if (error_) return;
        bram_.negedge();
        ofi_.negedge();
        const auto response = bram_.response_b();
        if (response.valid) {
            std::int32_t id = -1;
            if (!bram_ring_.pop(id) || !ofi_in_.free()) { fail(); return; }
            state_[static_cast<std::size_t>(id)].quote = response.data;   // quote as read back from the RAM
            ofi_in_.load(id, cycle + 1, 0);
        }
        if (ofi_.output_valid()) {
            std::int32_t id = -1;
            if (!ofi_ring_.pop(id) || !dma_q_.push(id)) { fail(); return; }
            state_[static_cast<std::size_t>(id)].ofi = ofi_.output();
            last_ofi_ = ofi_.output();
            cumulative_ofi_ += ofi_.output();
        }
    }

    void apply_to_book(MsgState& s) noexcept {
        // Anything but a clean update (or a counted book overflow, which the
        // demo reports and treats as a failed run) is a model error.
        const ApplyStatus st = applier_.apply(s.event);
        if (st != ApplyStatus::applied && st != ApplyStatus::book_overflow) fail();
        s.quote = applier_.quote();
    }

    void post_decision(std::uint64_t cycle) noexcept {
        const auto id = static_cast<std::size_t>(dma_.msg);
        const MsgState& s = state_[id];
        PcieTlp tlp{};
        tlp.address = 0x10000000ULL + 64ULL * id;
        tlp.byte_count = 16;
        tlp.tag = static_cast<std::uint32_t>(id);
        const auto put = [&tlp](std::size_t off, std::uint32_t v) {
            for (std::size_t i = 0; i < 4; ++i) tlp.payload[off + i] = static_cast<std::uint8_t>(v >> ((3 - i) * 8));
        };
        put(0, static_cast<std::uint32_t>(id));
        put(4, static_cast<std::uint32_t>(s.ofi));
        put(8, static_cast<std::uint32_t>(s.quote.bid_price));
        put(12, static_cast<std::uint32_t>(s.quote.ask_price));
        PcieTlp drained{};
        if (!dma_engine_post(tlp) || !dma_.msg_valid() || !dma_drain(drained) || drained.tag != tlp.tag) fail();
        latency_[id] = static_cast<std::uint32_t>(cycle - stream_.message(id).arrival_cycle + 1);
        dma_.msg = -1;
        ++completed_;
    }

    bool dma_engine_post(const PcieTlp& tlp) noexcept { return dma_.engine.post_write(tlp) && dma_.engine.msix_asserted(); }
    bool dma_drain(PcieTlp& out) noexcept {
        const bool got = dma_.engine.consume(out);
        dma_.engine.clear_msix();
        return got;
    }

    // DMA stage slot bundled with the DMA model instance.
    struct DmaStage : Slot {
        PcieGen4x16Dma engine{};
        [[nodiscard]] bool msg_valid() const noexcept { return msg >= 0; }
        [[nodiscard]] std::size_t posted_count() const noexcept { return engine.posted_count(); }
    };

    const SyntheticItchStream& stream_;
    Deserializer66b pcs_decoder_{};
    MacFramer mac_framer_{};
    Itch50Parser parser_{};
    ItchBookApplier applier_{};
    DualPortBram<BookQuote, 32, PipelineParams::bram_latency> bram_{};
    OfiDsp ofi_{};
    DmaStage dma_{};

    Slot pcs_{}, mac_{}, parse_{}, book_slot_{}, rd_{}, ofi_in_{};
    std::size_t mac_beats_total_ = 0;
    RingBuffer<std::int32_t, 16> bram_ring_{};
    RingBuffer<std::int32_t, 16> ofi_ring_{};
    RingBuffer<std::int32_t, 64> dma_q_{};
    std::array<MsgState, message_count> state_{};
    std::array<std::uint32_t, message_count> latency_{};
    std::size_t next_msg_ = 0;
    std::size_t completed_ = 0;
    std::int64_t cumulative_ofi_ = 0;
    std::int32_t last_ofi_ = 0;
    bool error_ = false;
};

} // namespace fpga_sim
