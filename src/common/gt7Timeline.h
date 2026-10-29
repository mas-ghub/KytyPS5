#pragma once
// GT7-INVESTIGATION (session 9, remove when done): latency chain of one GPU->CPU round trip (read faults only).
// 0 DrainEnd (GPU thread: GPU finished) -> 1 CmdDone (service command returns) -> 2 GuestWake (SendCommandSync returns in the
// guest thread) -> 3 FaultReturn (fault handler returns) -> 4 Submit (next vkQueueSubmit). Mean gaps printed every 10 s ("GT7TL").
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>

namespace Gt7Tl {
inline uint64_t Now() {
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
	                                 std::chrono::steady_clock::now().time_since_epoch()).count());
}
struct State {
	std::atomic<uint64_t> t[6] {}, state {0}, n {0}, ns[5] {}, next {0};
};
inline State& S() { static State s; return s; }
inline thread_local bool tl_woke = false;
inline bool Mark(int stage, bool restart = false) {
	auto& s = S();
	const auto now = Now();
	if (restart) { s.t[stage].store(now); s.state.store(stage + 1); return true; }
	if (s.state.load() != static_cast<uint64_t>(stage)) return false;
	s.ns[stage - 1] += now - s.t[stage - 1].load();
	s.t[stage].store(now);
	if (stage == 5) { s.n++; s.state.store(0); } else s.state.store(stage + 1);
	return true;
}
inline void DrainEnd() { Mark(0, true); }
inline void CmdDone() { Mark(1); }
inline void GuestWake() { if (Mark(2)) tl_woke = true; }
inline void BodyDone() { if (tl_woke) Mark(3); }
inline void FaultReturn() { if (tl_woke) { Mark(4); tl_woke = false; } }
inline void Submit() {
	auto& s = S();
	Mark(5);
	const auto t = Now();
	auto due = s.next.load();
	if (t >= due && s.next.compare_exchange_strong(due, t + 10'000'000'000ull) && due != 0) {
		const auto n = s.n.exchange(0);
		double g[5];
		for (int i = 0; i < 5; i++) g[i] = n ? s.ns[i].exchange(0) / 1e3 / n : 0.0;
		std::printf("GT7TL last10s: %llu round trips; us: drain-end->cmd-done %.0f, cmd-done->guest-wakes %.0f, guest-wakes->ReadMemory-end %.0f, ->handler-returns %.0f, ->next-submit %.0f\n",
		            (unsigned long long)n, g[0], g[1], g[2], g[3], g[4]);
		std::fflush(stdout);
	}
}
} // namespace Gt7Tl
