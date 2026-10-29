#ifndef KYTY_COMMON_PERFTRACE_H_
#define KYTY_COMMON_PERFTRACE_H_

// Opt-in, per-site timing for the graphics draw path plus a draw-structure repetition probe.
//
// Everything here is off unless the environment variable KYTY_PERF is set to a non-zero value.
// When off, the only cost is one relaxed atomic load per call site, which is why the call sites
// are left unconditional in the tree instead of living behind #ifdef.
//
// Deliberately not built on top of Common::DebugCounters: that API exposes totals and rates,
// which is enough for the performance overlay but cannot answer "how long does one draw take",
// "which phase of the draw dominates", or "how many draws repeat the previous draw's structure".
// These are fixed-bucket histograms so the numbers survive a single run instead of needing a
// profiler attached.
//
// Output goes to stderr every few seconds and once more at process exit, so it works whether or
// not the log file is redirected.

#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace Common::PerfTrace {

// Site being timed. Keep the names stable: the dump is meant to be diffed between runs.
enum class Phase : uint32_t {
	// Whole-draw buckets.
	DrawPrepare,   // RenderExecutor::PrepareDrawRenderState
	DrawTotal,     // ExecutePreparedDraw: resolve state through the vkCmdDraw*
	// PrepareDrawRenderState internals.
	GetGraphicsPrograms,
	ResolveColorTargets,
	ResolveDepthTarget,
	// ExecutePreparedDraw internals.
	PrepareBindings,      // PrepareBindings + PrepareGraphicsBindings
	PrepareVertexBuffers, // AcquireVertexBuffers + PrepareIndexBuffer
	GetGraphicsPipeline,
	AcquireRenderTargets,
	EmitDraw, // BeginRendering through the draw command
	// Fault handling, the GT7 readback path.
	FaultProcess,  // FaultManager::ProcessFaultBuffer, whole call
	FaultWait,     // the CommandScheduler::Wait inside it
	FaultReadback, // the whole deferred readback, submit to consumption
	Count
};

inline constexpr size_t PhaseCount = static_cast<size_t>(Phase::Count);
// ns values are bucketed by bit width, so 48 buckets covers up to ~2^47 ns (about 1.6 days).
inline constexpr size_t BucketCount = 48;
inline constexpr uint64_t DumpPeriodNs = 5'000'000'000ull;

struct Stat {
	std::atomic<uint64_t> count {0};
	std::atomic<uint64_t> total_ns {0};
	std::atomic<uint64_t> max_ns {0};
	std::atomic<uint64_t> buckets[BucketCount] {};
};

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
inline Stat g_stats[PhaseCount];
inline std::atomic<uint64_t> g_dump_stamp_ns {0};
inline std::atomic<uint64_t> g_dumps {0};

// Draw-structure probe: counts how many draws reuse a structure key already seen.
struct KeyProbe {
	std::atomic<uint64_t> draws {0};
	std::atomic<uint64_t> distinct {0};
	std::atomic<uint64_t> repeat_same_frame {0};
	std::atomic<uint64_t> repeat_earlier_frame {0};
	std::atomic<uint64_t> frame {0};
	std::atomic<uint64_t> draws_this_frame {0};
};
// NOLINTNEXTLINE
inline KeyProbe g_keys;

// Open-addressed key table. 128K slots at 16 bytes is 2MB, only touched when enabled, and large
// enough to hold several frames of a 3,600-draw scene without thrashing.
inline constexpr size_t KeySlots = 1u << 17;
struct KeySlot {
	uint64_t key   = 0;
	uint64_t frame = 0;
	bool     used  = false;
};
// NOLINTNEXTLINE
inline KeySlot g_key_slots[KeySlots];

// Fault page set, for the "which ranges are rewritten every frame" question.
inline constexpr size_t FaultSlots = 1u << 16;
struct FaultSlot {
	uint64_t page = 0;
	bool     used = false;
};
// NOLINTNEXTLINE
inline FaultSlot g_fault_slots[FaultSlots];
inline std::atomic<uint64_t> g_fault_invocations {0};
inline std::atomic<uint64_t> g_fault_count {0};
inline std::atomic<uint64_t> g_fault_distinct_pages {0};
inline std::atomic<uint64_t> g_fault_repeat_pages {0};

void Dump();

[[nodiscard]] inline bool Enabled() noexcept {
	static const bool enabled = [] {
		const char* value = std::getenv("KYTY_PERF");
		const bool  on    = value != nullptr && value[0] != '\0' && value[0] != '0';
		if (on) {
			std::atexit(&Dump);
		}
		return on;
	}();
	return enabled;
}

[[nodiscard]] inline uint64_t NowNs() noexcept {
	return static_cast<uint64_t>(
	    std::chrono::duration_cast<std::chrono::nanoseconds>(
	        std::chrono::steady_clock::now().time_since_epoch())
	        .count());
}

inline void AddSample(Phase phase, uint64_t ns) noexcept {
	if (phase >= Phase::Count) {
		return;
	}
	auto& stat = g_stats[static_cast<size_t>(phase)];
	stat.count.fetch_add(1, std::memory_order_relaxed);
	stat.total_ns.fetch_add(ns, std::memory_order_relaxed);
	auto previous = stat.max_ns.load(std::memory_order_relaxed);
	while (previous < ns &&
	       !stat.max_ns.compare_exchange_weak(previous, ns, std::memory_order_relaxed)) {
	}
	const auto bucket = static_cast<size_t>(std::bit_width(ns | 1ull));
	if (bucket < BucketCount) {
		stat.buckets[bucket].fetch_add(1, std::memory_order_relaxed);
	}
}

// RAII timer. Constructing it when disabled costs one static bool load.
class Scope {
public:
	explicit Scope(Phase phase) noexcept: m_phase(phase) {
		if (Enabled()) {
			m_start = NowNs();
		}
	}
	~Scope() {
		if (m_start == 0) {
			return;
		}
		const auto ns = NowNs() - m_start;
		AddSample(m_phase, ns);
		MaybeDump();
	}
	Scope(const Scope&)            = delete;
	Scope& operator=(const Scope&) = delete;
	Scope(Scope&&)                 = delete;
	Scope& operator=(Scope&&)      = delete;

private:
	static void MaybeDump() noexcept {
		const auto now  = NowNs();
		auto       last = g_dump_stamp_ns.load(std::memory_order_relaxed);
		if (last == 0) {
			// First sample: start the clock rather than dumping immediately, and keep the
			// start time as the reference for process-lifetime totals.
			(void)g_dump_stamp_ns.compare_exchange_strong(last, now, std::memory_order_relaxed);
			return;
		}
		if (now - last < DumpPeriodNs) {
			return;
		}
		if (!g_dump_stamp_ns.compare_exchange_strong(last, now, std::memory_order_relaxed)) {
			return;
		}
		Dump();
	}

	Phase    m_phase;
	uint64_t m_start = 0;
};

// Called once per guest frame flip, which is the frame boundary the guest sees.
inline void NewFrame() noexcept {
	if (!Enabled()) {
		return;
	}
	const auto previous_draws = g_keys.draws_this_frame.exchange(0, std::memory_order_relaxed);
	(void)previous_draws;
	g_keys.frame.fetch_add(1, std::memory_order_relaxed);
}

// One call per draw, after its state has been resolved. `key` must cover every input that
// decides the resolved draw state; see DrawStructureKey() in renderDraw.cpp.
inline void NoteStructureKey(uint64_t key) noexcept {
	if (!Enabled()) {
		return;
	}
	if (key == 0) {
		key = 0x9e3779b97f4a7c15ull; // keep 0 as the empty-slot marker
	}
	g_keys.draws.fetch_add(1, std::memory_order_relaxed);
	g_keys.draws_this_frame.fetch_add(1, std::memory_order_relaxed);
	const auto frame = g_keys.frame.load(std::memory_order_relaxed);

	auto slot_index = static_cast<size_t>((key * 0x9e3779b97f4a7c15ull) >> 47u) & (KeySlots - 1u);
	for (size_t probe = 0; probe < 8; ++probe, slot_index = (slot_index + 1u) & (KeySlots - 1u)) {
		auto& slot = g_key_slots[slot_index];
		if (!slot.used) {
			slot.used  = true;
			slot.key   = key;
			slot.frame = frame;
			g_keys.distinct.fetch_add(1, std::memory_order_relaxed);
			return;
		}
		if (slot.key != key) {
			continue;
		}
		if (slot.frame == frame) {
			g_keys.repeat_same_frame.fetch_add(1, std::memory_order_relaxed);
		} else {
			g_keys.repeat_earlier_frame.fetch_add(1, std::memory_order_relaxed);
			slot.frame = frame;
		}
		return;
	}
	// Table pressure: count it as distinct rather than growing without bound.
	g_keys.distinct.fetch_add(1, std::memory_order_relaxed);
}

// One call per pending page fault address. Reports distinct and recurring pages, and the
// recurring set is what a GPU-clean-range fix would target.
inline void NoteFaultAddress(uint64_t address) noexcept {
	if (!Enabled()) {
		return;
	}
	const auto page = address >> 12u;
	g_fault_count.fetch_add(1, std::memory_order_relaxed);

	auto slot_index = static_cast<size_t>((page * 0x9e3779b97f4a7c15ull) >> 48u) & (FaultSlots - 1u);
	for (size_t probe = 0; probe < 8; ++probe, slot_index = (slot_index + 1u) & (FaultSlots - 1u)) {
		auto& slot = g_fault_slots[slot_index];
		if (!slot.used) {
			slot.used = true;
			slot.page = page;
			g_fault_distinct_pages.fetch_add(1, std::memory_order_relaxed);
			return;
		}
		if (slot.page == page) {
			g_fault_repeat_pages.fetch_add(1, std::memory_order_relaxed);
			return;
		}
	}
}

inline void NoteFaultInvocation() noexcept {
	if (!Enabled()) {
		return;
	}
	g_fault_invocations.fetch_add(1, std::memory_order_relaxed);
}

namespace Detail {

[[nodiscard]] inline const char* PhaseName(Phase phase) noexcept {
	switch (phase) {
		case Phase::DrawPrepare: return "draw.prepare";
		case Phase::DrawTotal: return "draw.total";
		case Phase::GetGraphicsPrograms: return "draw.programs";
		case Phase::ResolveColorTargets: return "draw.color";
		case Phase::ResolveDepthTarget: return "draw.depth";
		case Phase::PrepareBindings: return "draw.bindings";
		case Phase::PrepareVertexBuffers: return "draw.vertexbuf";
		case Phase::GetGraphicsPipeline: return "draw.pipeline";
		case Phase::AcquireRenderTargets: return "draw.framebuf";
		case Phase::EmitDraw: return "draw.emit";
		case Phase::FaultProcess: return "fault.process";
		case Phase::FaultWait: return "fault.wait";
		case Phase::FaultReadback: return "fault.readback";
		case Phase::Count: break;
	}
	return "?";
}

// Upper bound of a bit-width bucket, used as the percentile estimate.
[[nodiscard]] inline uint64_t BucketBoundNs(uint64_t bucket) noexcept {
	if (bucket == 0) {
		return 0;
	}
	if (bucket >= 63) {
		return UINT64_MAX;
	}
	return (1ull << bucket) - 1ull;
}

[[nodiscard]] inline uint64_t Percentile(const Stat& stat, double fraction) noexcept {
	const auto count = stat.count.load(std::memory_order_relaxed);
	if (count == 0) {
		return 0;
	}
	const auto target   = static_cast<uint64_t>(static_cast<double>(count) * fraction);
	uint64_t   running  = 0;
	for (size_t bucket = 0; bucket < BucketCount; ++bucket) {
		running += stat.buckets[bucket].load(std::memory_order_relaxed);
		if (running >= target) {
			return BucketBoundNs(bucket);
		}
	}
	return stat.max_ns.load(std::memory_order_relaxed);
}

} // namespace Detail

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
inline void Dump() {
	if (!Enabled()) {
		return;
	}
	const auto dump_index = g_dumps.fetch_add(1, std::memory_order_relaxed) + 1;
	std::fprintf(stderr,
	             "\n=== KYTY_PERF dump #%llu ===\n"
	             "%-16s %10s %12s %10s %10s %10s\n",
	             static_cast<unsigned long long>(dump_index), "phase", "count",
	             "total ms", "mean us", "p50 us", "p99 us");

	for (size_t index = 0; index < PhaseCount; ++index) {
		const auto& stat  = g_stats[index];
		const auto  count = stat.count.load(std::memory_order_relaxed);
		if (count == 0) {
			continue;
		}
		const auto total = stat.total_ns.load(std::memory_order_relaxed);
		const auto mean  = static_cast<double>(total) / static_cast<double>(count);
		std::fprintf(stderr, "%-16s %10llu %12.2f %10.2f %10.2f %10.2f\n",
		             Detail::PhaseName(static_cast<Phase>(index)),
		             static_cast<unsigned long long>(count),
		             static_cast<double>(total) / 1e6, mean / 1e3,
		             static_cast<double>(Detail::Percentile(stat, 0.50)) / 1e3,
		             static_cast<double>(Detail::Percentile(stat, 0.99)) / 1e3);
	}

	const auto draws        = g_keys.draws.load(std::memory_order_relaxed);
	const auto distinct     = g_keys.distinct.load(std::memory_order_relaxed);
	const auto same_frame   = g_keys.repeat_same_frame.load(std::memory_order_relaxed);
	const auto earlier      = g_keys.repeat_earlier_frame.load(std::memory_order_relaxed);
	const auto frames       = g_keys.frame.load(std::memory_order_relaxed);
	if (draws != 0) {
		std::fprintf(stderr,
		             "\ndraw structure: %llu draws, %llu distinct keys (%.1f%% of keys reused), "
		             "%llu repeats same frame, %llu repeats from an earlier frame "
		             "(%.1f%% of all draws), %llu frames\n",
		             static_cast<unsigned long long>(draws),
		             static_cast<unsigned long long>(distinct),
		             distinct != 0 ? 100.0 - (static_cast<double>(distinct) * 100.0 /
		                                      static_cast<double>(draws))
		                           : 0.0,
		             static_cast<unsigned long long>(same_frame),
		             static_cast<unsigned long long>(earlier),
		             static_cast<double>(earlier) * 100.0 / static_cast<double>(draws),
		             static_cast<unsigned long long>(frames));
		if (frames != 0) {
			std::fprintf(stderr, "draw structure: %.1f draws/frame\n",
			             static_cast<double>(draws) / static_cast<double>(frames));
		}
	}

	const auto fault_invocations = g_fault_invocations.load(std::memory_order_relaxed);
	if (fault_invocations != 0) {
		std::fprintf(stderr,
		             "\nfaults: %llu invocations, %llu pages, %llu distinct pages, "
		             "%llu recurring page hits (%.1f%%)\n",
		             static_cast<unsigned long long>(fault_invocations),
		             static_cast<unsigned long long>(g_fault_count.load(std::memory_order_relaxed)),
		             static_cast<unsigned long long>(
		                 g_fault_distinct_pages.load(std::memory_order_relaxed)),
		             static_cast<unsigned long long>(
		                 g_fault_repeat_pages.load(std::memory_order_relaxed)),
		             g_fault_count.load(std::memory_order_relaxed) != 0
		                 ? static_cast<double>(
		                       g_fault_repeat_pages.load(std::memory_order_relaxed)) *
		                       100.0 /
		                       static_cast<double>(
		                           g_fault_count.load(std::memory_order_relaxed))
		                 : 0.0);
		std::fflush(stderr);
	}
	std::fflush(stderr);
}

} // namespace Common::PerfTrace

#endif // KYTY_COMMON_PERFTRACE_H_
