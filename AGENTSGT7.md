# Gran Turismo 7 on KytyPS5 — complete handoff (read this first for GT7 work)

**UPDATE 2026-10-17 (session 10, Claude Sonnet 5.5): read the END OF DAY STATE at the end of `EMULATOR-SPEED-PLAYBOOK.md` first.** Key result: GT7's ~8 fps is a LATENCY LOOP (game submit -> CP/recorder/submit worker -> dependent GPU step -> completion event); a named wait graph (xperf, `work\etw_wait.ps1`, scheduled task `KytyEtwWait`) shows nothing saturated (CP 26 % busy, game threads ~15 % of one core, GPU 40-60 %). Output-off ceiling ~10 fps. Measured ~0 and NOT worth retrying: GPU power 240 W, clocks, ballot CSE (`KYTY_BALLOT_CSE`), GT7 probes (now gated by `KYTY_GT7_PROBES`), guest CPU sets, timer resolution, Nsight sampling (0 samples), async compute. New tools: named OS threads (`guest:<name>`, Thread_Gpu, CP recorder, ...), `cpsample.py modsplit/probe`, `wait_graph2.py`, live flags `_skip_draws.flag` / `_null_audio.flag`, `output_off_ab.ps1`. Uncommitted: `hangWatchdog` inline fast path (A/B interrupted by shutdown, UNMEASURED: keep only if it beats +-1-2 fps noise).

> **NEWEST WORK (2026-10-16, Claude Sonnet 5.5): read `EMULATOR-SPEED-PLAYBOOK.md` FIRST** (methodology, toolbox, every measurement, what to try next, pitfalls), then section 16 of this file
> (dated log). Since the notes below were written the 3D scene went from ~6.5 to ~9 fps (checked speculative front reads +20 %, GPU 240 W +10 %), the GPU crash was fixed, the controller works,
> the intro movie is skipped, PSN popups are suppressed and `gt7boot.ps1` boots to the 3D scene unattended. Sections 0-15 below are older context and partly superseded.

> **LATEST HANDOFF IS §15 (end of 2026-10-15) — READ THAT FIRST, THEN THIS FILE.** Summary: the
> unattended boot to the 3D scene is **close but NOT reliable** (presses land on auto-advancing pages
> - Mark reported it twice), and the 3D scene still runs at **9 fps**, which is the real goal.
> §15.1 lists what is solved and must not be redone; §15.2 is the exact work for next session
> (measure each screen's fingerprint with `work\bootrecord.py`, then gate every press on it).

Written 2026-10-15 for ANY model/engineer who has to continue this without the earlier chat (e.g. a smaller/faster
model). It is self-contained: goal, state, how to build/run/test, every change made and why, tools, findings, pitfalls,
next steps. **Keep it updated after EVERY change** (see §12 "How to keep this file current"). General emulator history
(Astro Bot, performance work) is in `AGENTSKYTY.md`; machine/GPU context in `C:\Users\mark\AGENTS.md`.

---
## START HERE — for a new model/engineer taking over (Mark will say "read the MD and continue")

**Your job:** read this whole file (it is the single source of truth), then continue the work from the NEXT STEPS below,
using the findings, tools and dead-ends already recorded so you do not repeat anything. Claude (the previous model) did
sessions 5-6; you are session 7+.

**Procedure (do this in order, every session):**
1. Read this file fully (§0 state, §3 run, §4 tools, §10 next steps, §14 playbook, §13 log). Read `AGENTSKYTY.md` top lines only if you touch Astro Bot.
2. Check reality before trusting the text: is `kyty_gt7` running (`Get-Process kyty_gt7`)? does `C:\Users\mark\KytyPS5-jetsku\kyty_gt7.exe` exist? `git -C C:\Users\mark\KytyPS5-src status --short` should show only `colorRenderTarget.cpp` + `pipelineCache.cpp` modified (unless committed since).
3. Tell Mark in 3 lines what you understood and what you will do first; then start. Don't ask questions the file answers.
4. Work one problem at a time with the §14 playbook. Mark runs the slow game parts and pastes results; give ONE command per block.
5. After EVERY change/finding update this file (§12): §13 log line, §6 table, patch file, dead ends. If you do not write it down it did not happen. Sign your log lines with your model name so the next reader knows who found what.
6. Never overwrite `kyty_emulator.exe` (Astro Bot). Never commit unless Mark asks. Never run destructive git commands without his OK. The human writes any upstream issue/PR text.

**State at handoff (2026-10-15, end of Claude's session 6):** GT7 boots, renders, text readable, tutorial DRIVEABLE. The EQAA crash
is FIXED (§6 #7, revised to fragment count). Tutorial/menus run 41-58 fps with warm caches; the **race scene runs 6-11 fps**,
limited by ~1,400 guest write faults/frame (183-209 ms/frame). Knobs `KYTY_TEXTURE_FAULT_FAST_PATH`/`KYTY_HOT_PAGE_MAX` = dead end.
Uncommitted: #6 + #7 (patch file `gt7-uncommitted-2026-10-15.patch`). Test exe `kyty_gt7.exe`. Last run command: §3.

**NEXT STEPS, in priority order (pick up at 1):**
1. ~~**Tracy profile of the race scene**~~ **DONE (§13 session 7, 2026-10-15).** Result: the race scene is **command-processor-bound** —
   thread 1 (CommandProcessor) is 99 % busy at ~8 fps (~119 ms/frame); the DrawPrep workers are idle. The CP splits as `Process` self
   39 ms/frame + `Resolve` self 33 ms/frame (contains the `_mm_pause` starvation spin) + draw commit ~46 ms/frame. The guest write faults
   are real but NOT the limiter (6 threads x ~19 % busy). Follow-ups, in order:
   * **1a. `KYTY_CP_COMMIT_STATS` run — DONE (§13 session 7, run `gt7cp3`).** Race scene: commit ≈ 58 ms + gap ≈ 93 ms ≈ 151 ms/frame;
     the gap is dominated by **waiting on the GPU** (`headwait.barrier` ≈ 34 ms + `gpuwait.drain` ≈ 24 ms), NOT front-end starvation
     (`starved` ≈ 2 ms). Worst class: **`instance` draws, 388/frame at 210 µs each = 81.5 ms (51 %)**; then `runmiss` 1,700/f = 33 ms (21 %),
     `pipeline` 696/f = 25 ms (16 %). GT7 never continues a draw run (`cont` 11/f), so DrawRun's delta commit is useless here.
   * **1b. Finer zones inside `Process`/`Resolve` self** — now partly covered by the `gapsplit` above; go deeper only if the barrier/drain path needs it.
   * **1c. The GT7-specific lead:** the fault map shows each guest fault costs 133 µs of which **84 µs is a GPU readback** (`flush`) — GT7's
     guest rewrites ~1,000 GPU-owned pages/frame. A source-level fix (keep those ranges GPU-clean / not write-tracked) would be the big win;
     find them with the fault map (`KYTY_FAULT_MAP=1`, `AGENTSKYTY.md` §11 log 2026-10-13 session 2). Hot regions recorded in §13 session 7.
   * **1d. Barrier/drain waits — RESOLVED as NOT the limiter (§13 `gt7ab`/`gt7ab2`).** `KYTY_READBACK_FLUSH_SIDE=1` was A/B'd twice with the live-switch
     mechanism: it verifiably converts the current-data readbacks to side copies (`current` drains -> 0.00/frame) and drops `gpuwait.drain` (e.g.
     26.5 -> 6.0 ms at matched load), **but the frame time does not change** — the wait is re-attributed (commit rises, gap falls, same `wall`).
     **Do not adopt it.** The `gt7ab` device loss did not reproduce in `gt7ab2`. Next real targets: the `instance` class's ~30-40 us/draw `markbindings`
     (source change), the per-draw `headwait.barrier` hand-off, and GT7's 3,600-4,700 draws/frame (it never continues a run — `cont` ~11/f).
     Live-switch A/B recipe: `.\gt7fm.ps1 -Tag X -NoFaultMap -NoProfile -CpStats -Live` + rewrite `work\live.txt`; compare only windows with the
     same `frames/10 s` and align on the `Live: seq N applied at CP flip ...` lines.
2. **Save data check:** does GT7 persist settings? Finish the tutorial race, quit, relaunch: are control/assist choices remembered?
   Mark reports re-choosing controls each launch. `_SaveData\PPSA01317` only had `sce_sdmemory\1000\memory.dat` (08:35) and LIVERYIV/DRFILEIV.
   Grep the guest log (`--printf-direction File`) for failed `sceSaveData*` calls; look at `src/libs/` save-data implementation.
3. **Commit #6/#7** when Mark says so; tag `restore-point-gt7-eqaa-<date>`.
4. **ErrorDeviceLost / "3D WIDTH ZT Violation"** (§9) if it recurs: use the `--- Device loss` block.
5. **Beyond the tutorial:** main menu, a real race, car showroom; note each new crash's first `--- Error ---` line and fix it (§14).
6. Unimplemented imports list (§10 item 6) — only if a game feature visibly needs one.

---
## 0. TL;DR (state at the end of the last session entry in §13)

### Current Performance (2026-10-29 session)
- **Menus**: 59-60 fps (stable)
- **3D scene (Alsace Village)**: 5-15 fps real-time (log shows 4.3-27.7 fps 60s average depending on scene, user reports 5-15 fps)
- **Fault cost**: 23.58 ms/frame (307 faults at 76.8us) with safe write-track-skip
- **Main bottleneck**: per-draw commit (~58ms/frame) + GPU readback drains (~30ms/frame)
- **Write-track-skip**: safe version active (non-GPU-dirty pages only), limited effect
- **XFrameReuse**: active (47.8% revalidation, +27% fps)
- **Pre-warm**: blocking (3000+ shaders compiled upfront, ~30-60s wait)
- **strata**: CLOSED (was consuming 48GB RAM, causing heavy paging)

### Code Changes Made This Session

| File | Change | Status |
|------|--------|--------|
| `src/graphics/host_gpu/renderer/pipeline/pipelineCache.cpp` | Pre-warm defaults changed 0→1 (KYTY_SHADER_PRECOMPILE, KYTY_SHADER_PRECOMPILE_WAIT) | REVERTED to original (0) — caused "not responding" during startup |
| `src/common/u59Preset.cpp` | Added KYTY_SHADER_PRECOMPILE_WAIT to MarkerKeys | REVERTED |
| `u59-preset.json` | Added KYTY_SHADER_PRECOMPILE_WAIT: "1" | REVERTED |
| `src/graphics/host_gpu/renderer/cache/bufferCache.cpp` | Safe write-track-skip (skip non-GPU-dirty pages in manual ranges) | ACTIVE (in deployed binary) |
| `src/graphics/host_gpu/renderer/renderDraw.cpp` | Cross-frame m_run keep (BeginDrawRun) | REVERTED (didn't help) |
| `src/graphics/host_gpu/renderer/renderDraw.cpp` | DrawRunCandidate certificate bypass (m_run_cross_frame) | REVERTED (didn't help) |
| `src/graphics/host_gpu/renderer/renderDraw.cpp` | DrawRunCommandUnchanged cross-frame bypass | REVERTED (didn't help) |
| `src/graphics/host_gpu/renderer/render.h` | CrossFrameEntry / XFrameCacheEntry structs | REVERTED |
| `src/graphics/host_gpu/renderer/renderDraw.cpp` | DrawRunRecallCandidate cross-frame bypass | REVERTED (didn't help) |

### What Needs Doing for 50-60fps (Full Cross-Frame Draw State Reuse)

**The fundamental bottleneck**: 3,600 draws/frame × ~16us = 57.6ms per frame. 87% of draws repeat structure frame-to-frame. Cross-frame caching would skip re-resolution for repeated draws.

**Required implementation (2-3 weeks):**

1. **Cross-frame cache storage** (`renderDraw.h` or `pipelineCache.h`)
   - Hash map: structure_key → resolved state (targets, textures, samplers, pipeline)
   - Bounded by size (LRU eviction, ~64MB)
   - Store Vulkan object references (ImageId, views) — already cached in texture/pipeline caches

2. **Cache lookup in PrepareDrawRenderState** (`renderDraw.cpp` ~line 2108)
   - After RefreshShaders, compute structure key from context registers
   - Check cache. If hit and image epoch unchanged → reuse cached state, skip resolution
   - If miss → resolve normally, store in cache

3. **Invalidation** (`image.h` image state epoch)
   - Clear cache when image state epoch bumps (texture/attachment mutation)
   - Clear cache on scene transition (draw count changes significantly)
   - Clear cache when pipeline cache is invalidated

4. **Verify mode** (`KYTY_CROSS_FRAME_CACHE=verify`)
   - Compare cached state with freshly resolved state
   - Count mismatches, log first N differences
   - Must show 0 mismatches before enabling

5. **Files to modify**:
   - `src/graphics/host_gpu/renderer/renderDraw.h` — add cache struct to RenderExecutor
   - `src/graphics/host_gpu/renderer/renderDraw.cpp` — PrepareDrawRenderState (lookup + store)
   - `src/graphics/host_gpu/renderer/drawPrep/drawRun.cpp` — StructureKey (already computes key)
   - `src/graphics/host_gpu/renderer/image/image.h` — image state epoch (already exists)
   - `src/graphics/host_gpu/renderer/pipeline/pipelineCache.h` — cache storage (optional)

**Alternative shorter-term improvements:**
- Reduce GPU readback drains (153ms/frame for READ current) — GT7's GPU→CPU round trips
- Skip hot shader (KYTY_GT7_SKIP_CS=618f2427a8040fcf) — +19% but may break rendering
- Increase draw prep workers beyond 12 — marginal improvement
- Play through game once to build comprehensive journal — pre-warm covers all scenes

**Current best config for GT7:**
- Exe: kyty_int21p.exe (original int21 build, no my changes)
- Preset: u59-preset.json (all defaults)
- Env: KYTY_HOST_INPUT_ONLY=0, close strata first
- Result: 38-42 fps in 3D scene (without strata), 55-60 fps in menus
- Fault cost: 11.07 ms/frame (138 faults at 79.9us)

## 0. TL;DR (state at the end of the last session entry in §13)

* **Goal:** run Gran Turismo 7 (PS5, title ID **PPSA01317**, v01.680) on the KytyPS5 emulator (Jetsku fork, int19.1 base) on
  Mark's PC (Ryzen 9 5950X, RTX 3090, Win 11). Compat DB said "doesn't boot" — we got it **booting, rendering 3D, readable text, tutorial driveable**.
* **Works now:** boots; first-run dialogs; menus with correct text; the 3D tutorial and race scene (Alsace Village) render and
  can be driven; Xbox controller input works. Speed: 41-58 fps in menus/tutorial with warm caches, **6-11 fps in the race scene**
  (command-processor-bound, not the write faults — §13 session 7). Slow but driveable.
* **Open problems (in priority order):** (1) race-scene performance: **command-processor-bound** — thread 1 is 99 % busy;
  at 6.3 fps the frame is ~151 ms of CP time = **commit ~58 ms + gap ~93 ms**, and the gap is **waiting on the GPU**
  (`headwait.barrier` ~34 ms + `gpuwait.drain` ~24 ms), not front-end starvation (`starved` ~2 ms). Worst draw class: **`instance`,
  388/frame at ~210 us each = 51 %**; GT7 never continues a draw run (§13 session 7, START HERE steps 1/1a);
  (2) settings not remembered between launches (save data, step 2); (3) rare GPU crash `ErrorDeviceLost` = NVIDIA "3D WIDTH ZT
  Violation" (§9); (4) Astro Bot must not regress (§7). (The EQAA crash is FIXED, §6 #7.)
* **Source:** `C:\Users\mark\KytyPS5-src`, branch `gt7-spike`. Committed: `e6e1f12a`, `ada5ab05`. **Uncommitted** (also saved as
  `work\gt7-uncommitted-2026-10-15.patch`): program-cache cap env var + EQAA fix. Commit ONLY when Mark asks.
* **Test exe:** `C:\Users\mark\KytyPS5-jetsku\kyty_gt7.exe` (built from `gt7-spike`). **Never overwrite**
  `kyty_emulator.exe` (Astro Bot daily driver, int19.1 PGO+LTO).

---
## 1. Who/what/where (facts you must not guess)

| Thing | Value |
|---|---|
| User | Mark. PC `MARKSDESKTOP`. Runs Windows 11 24H2, shell is **non-elevated**. Prefers to run slow commands himself and paste results. |
| GT7 dump | `D:\GT7\[SuperPSX]-Gran.Turismo.7-PPSA01317-USA-Game (v01.680) [APR-EMU]-PS5\[SuperPSX]-Gran.Turismo.7-PPSA01317-USA-Game (v01.680) [APR-EMU]-PS5\[SuperPSX]-Gran.Turismo.7-PPSA01317-USA-Game (v01.680) [APR-EMU]-PS5` (same name nested 3 deep; the innermost has `eboot.bin`, `sce_sys`, `sce_module`, `contents`, `fakelib`, `ampr_emu.index`, `pdiWheel_*.prx` stubs) |
| Emulator runtime folder | `C:\Users\mark\KytyPS5-jetsku\` (working dir; holds `_PipelineCache\`, `_RenderDoc\`, `u59-preset.json`, exes) |
| Source repo | `C:\Users\mark\KytyPS5-src` (junction `C:\kyty-src` -> same dir; the PGO build MUST be built through the junction) |
| Notes/scripts | `C:\Users\mark\KytyPS5\work\` (this file, `AGENTSKYTY.md`, `play.ps1`, `gt7-build.bat`, `capture.ps1`, `measure.ps1`, `rdoc_*.py`) |
| Logs | `C:\Users\mark\KytyPS5\logs\` (`<tag>.out` = emulator stdout, `<tag>.err`, `<tag>_guest.txt` = guest printf log when requested) |
| Astro Bot (other game) | `C:\Users\mark\KytyPS5\games\PPSA21564-app` — dump size is normal (148 GB), nothing to prune |
| GPU power cap | 200 W (`nvidia-smi -pl 200`, re-applied at boot by a task). Fine: workloads are CPU-bound. GPU had a hardware-fault history (see `AGENTSGPU.md`); the GT7 crashes so far are **software**, not that fault. |
| Memory rule | close the AI workload `strata` before running (emulator commits ~14 GB; GT7 working set reaches ~11 GB) |
| Disk | C: ~30 GB free. `_PipelineCache\PPSA01317.*` grows (programs.bin ~700 MB, .bin driver cache ~350 MB). Delete regenerable big files (Tracy CSVs) if low. |

---
## 2. How to build (always works this way)

1. **Close the emulator first** (the build copies over `kyty_gt7.exe`; a running exe blocks it):
   `Get-Process kyty_gt7 -ErrorAction SilentlyContinue | Stop-Process -Force`
2. Run (classic PowerShell; ~5-10 min incremental, a full rebuild is 20-30 min):
   `Start-Process cmd -ArgumentList '/c','C:\Users\mark\KytyPS5\work\gt7-build.bat > C:\Users\mark\KytyPS5\logs\build_gt7.log 2>&1' -Wait`
3. Check: `gc C:\Users\mark\KytyPS5\logs\build_gt7.log -Tail 3` must end with `=== done: kyty_gt7.exe ===`.
   On failure read the last ~30 lines for the compiler error.
* `gt7-build.bat` calls `pgo-build-191.bat` (LLVM 22.1.3 clang-cl, PGO profile `u59-int19-sg-1.profdata`, LTO, `--parallel 6`,
  build dir `KytyPS5-src\_build_191`) then copies `_build_191\kyty_emulator.exe` -> `KytyPS5-jetsku\kyty_gt7.exe`.
* Do NOT launch the build from Git Bash (`cmd /c` silently does nothing there); use PowerShell `Start-Process cmd`.
* Source edits are compiled from `C:\kyty-src` (junction) — editing `C:\Users\mark\KytyPS5-src` is the same files.

---
## 3. How to run GT7 (exact commands)

Classic PowerShell (not Windows Terminal). **One command per paste; never paste two commands on one line** (Mark's pastes
have merged lines before).

```powershell
cd C:\Users\mark\KytyPS5\work
.\play.ps1 -Exe kyty_gt7.exe -Tag gt7x -Extra 'KYTY_PROGRAM_CACHE_MAX_MB=4096','KYTY_PIPELINE_FAST_FIRST=0','KYTY_PIPELINE_PREFETCH=1','KYTY_PIPELINE_PREFETCH_THREADS=4','KYTY_DEVICE_FAULT_DIAGNOSTICS=1' -Game 'D:\GT7\[SuperPSX]-Gran.Turismo.7-PPSA01317-USA-Game (v01.680) [APR-EMU]-PS5\[SuperPSX]-Gran.Turismo.7-PPSA01317-USA-Game (v01.680) [APR-EMU]-PS5\[SuperPSX]-Gran.Turismo.7-PPSA01317-USA-Game (v01.680) [APR-EMU]-PS5'
```
`play.ps1` parameters: `-Exe` (exe in the jetsku folder), `-Tag` (log file base name; use a NEW tag per run so logs are not
overwritten), `-Extra 'K=V','K2=V2'` (environment overrides, array of separate strings), `-Game` (path; it is quoted for you),
`-EmuArgs '--flag','value'` (emulator command-line args), `-NoPin` (skip the chiplet pin). It applies Jetsku's
`u59-preset.json`, pins the pipeline to CPUs 16-31 and guest threads to 0-15 (measured +20% on Astro Bot), sets High priority.

Useful `-EmuArgs`: `'--printf-direction','File','--printf-output-file','C:\...\x_guest.txt'` (guest log; ~200-500 MB, needed
for font/metrics debugging), `'--rd'` (RenderDoc capture on; press **F1** in the emulator window), `'--profile'` (Tracy).

**How the AGENT drives this route (verified 2026-10-15, session 7 — no human needed; see `work\gt7ctl.ps1`):**
1. Emulator dialog "Sign in to PlayStation Network … unavailable" -> **`winkey Return`** (it is the *emulator's* own dialog: the pad map never
   applies to it, and Escape alone does nothing — Return is its default button).
2. GT7 dialog "Failed / You cannot use network functions…" with **OK** -> **Cross** (`J`), held >=150 ms.
3. Intro cinematics -> Cross advances/skips; they also auto-advance.
4. **Display Settings** (crosshair, "Make sure you are facing straight at the television…", Start) -> **D-pad Down + Right held together, then Cross**.
5. **Display Settings / Black Point Adjustment** (slider + Back/Next) -> **Down + Right together, then Cross**.
6. **Select the graphics priority for PlayStation®5** (Prioritize Frame Rate / Prioritize Ray Tracing-Resolution) -> **Cross** (Frame Rate is
   preselected; we want Frame Rate for speed work).
7. **Display Settings / Your Settings** (Resolution 4K, HDR Off, Graphics Priority = Prioritize Frame Rate, Back/Close) -> **Right, then Cross** (Close).
8. Then: Music Rally intro -> OK (Cross); "INITIALIZING" can sit at 0 fps for minutes while shaders compile (normal, cached after the first time);
   "Welcome to Gran Turismo 7" tutorial: Select Steering Controls -> **Left stick**, Assist Preset -> **Beginner**.

**Input rules that matter (all verified):** use `key <K> down` / wait **>=150 ms** / `key <K> up` — never the file channel's `tap`, which emits
down+up in the same millisecond and is invisible to a 60 Hz game. Force the window to the foreground before each press (the helper does this
automatically; losing focus mid-hold releases the key). D-pad = arrow key names (`Down`, `Right`, `Up`, `Left`), sticks = `W/A/S/D`, Cross = `J`.
The mouse remains the one channel that does NOT work (see §11).
  File input `_kyty_input.txt` exists but needs window focus.

---
## 4. Tools to look at a running/finished emulator (what each is for)

| Need | Tool / command |
|---|---|
| Screenshot the window (I can then read it) | `& C:\Users\mark\KytyPS5\work\capture.ps1 -Proc kyty_gt7 -Out C:\...\x.png` then view the PNG |
| **Drive the game myself (agent)** | `C:\Users\mark\KytyPS5\work\gt7ctl.ps1 -Steps 'focus','winkey Escape','wait 2','tap J','hold W 1.5','shot logs\x.png'`. Steps: `focus`, `tap <KEY>`, `down/up <KEY>`, `hold <KEY> <s>`, `shake n|m <s>`, `winkey Escape|Return|Space|F1`, `wait <s>`, `shot <path>`. **Pad keys via the file channel need NO focus; the emulator's own dialogs take `winkey Escape`/`Return`; injected mouse does NOT work** (see §11). `key.ps1` = pad-key only; `click.ps1`/`click2.ps1` = mouse (does not work on the emulator's UI). |
| Is it alive / frame progress | window title has `frame: N, fps: F`: `(Get-Process kyty_gt7).MainWindowTitle`; sample twice a few seconds apart. `fps: 0` for minutes + high CPU = compiling, not frozen |
| Clean fps number | `& C:\Users\mark\KytyPS5\work\measure.ps1 -Proc kyty_gt7 -Label x -Seconds 30` (default `-Proc` is kyty_emulator!) |
| Emulator health lines | `logs\<tag>.out`: `Kyty fault cost: last N s, X fps ...` (write-tracking cost per frame, "slow level"), `Pipeline fast-first ...` (compile stalls; absent when FAST_FIRST=0), `Program cache: saved N records (MiB)`, `Pipeline prefetch: 4 compile workers` |
| Why it closed | `logs\<tag>.out` tail: look for `--- Error ---` (emulator EXIT) or `--- Device loss` (GPU). Then Windows System log `nvlddmkm` events (see §9) |
| Tracy CPU trace (which thread is the limit) | launch with `-EmuArgs '--profile'`; in a 2nd window: `$t='C:\Users\mark\KytyPS5-src\_build_tracytools\capture\tracy-capture.exe'; $o='C:\Users\mark\KytyPS5\logs\tracy_x.tracy'; & $t -o $o -s 30 -f` (one line!). Export: `tracy-csvexport.exe -e -u <trace> > file.csv` then `python -I tracy_analyze.py file.csv out.txt 12` (per-thread busy + top zones + top zones by source line). CSV ~700 MB per 30 s: delete afterwards. **Pitfalls:** PowerShell 5.1 `>` writes the CSV as **UTF-16LE** (analyzer sniffs it now); csvexport 0.14.1 emits **7 columns** (`name,src_file,src_line,ns_since_start,exec_time_ns,thread,value`) — `tracy_analyze.py` is header+encoding driven. `--profile` is a startup flag, so the emulator must be launched with it. |
| RenderDoc frame capture + scripted analysis | `-EmuArgs '--rd'`, press F1 once in the window; capture lands in `KytyPS5-jetsku\_RenderDoc\*.rdc`. Analyse with RenderDoc's embedded Python: `Start-Process 'C:\Program Files\RenderDoc\qrenderdoc.exe' -ArgumentList '--python','C:\Users\mark\KytyPS5\work\rdoc_<x>.py' -PassThru` and read `logs\rdoc\<x>.txt`. Scripts: `rdoc_textures.py` (texture list + PNGs), `rdoc_geom.py` (every draw's geometry), `rdoc_mesh.py` (mesh-shader outputs = GT7 glyph quads), `rdoc_quads.py` (all glyph quad positions). The capture path is hard-coded at the top of each script — edit it. RenderDoc 1.36; its auto-update was disabled in `%APPDATA%\qrenderdoc\UI.config` (backup `UI.config.bak-claude`) because the update prompt blocked scripts |
| Read on-screen TEXT without OCR | In the guest log, lines `FontMetrics: code=0x.. scale=..` and `FontRender: code=0x..` list the characters GT7 asked for, in order; repeated letters are cached so words appear with letters missing. Scripts in the session scratchpad did this (`stream.py`); easy to redo |
| Identify an unknown PS5 function name from its NID | NID = base64(first 8 bytes of sha1(name + salt 518D64A635DED8C1E6B039B1C3E55230), REVERSED), no '=' padding. Hash candidate names (`sceFont...`). Verify the recipe first on a known pair (`sceFontGetHorizontalLayout` = `imxVx8lm+KM`) |

Python gotchas on this PC: use `C:\Users\mark\AppData\Local\Programs\Python\Python311\python.exe -I -X utf8 script.py`; `-I` ignores
env vars (so `PYTHONIOENCODING` does nothing; use `-X utf8` and avoid printing non-ASCII); keep scripts OUT of dirs with untrusted data.

---
## 5. The GT7 boot chain: problems found and the fix for each (chronological)

1. **Compat DB "doesn't boot"** (issue #694: `elf is not valid pdiWheel_0.prx`). On int19.1 our build did NOT hit it; the game
   boots to its menus. (The 360-byte `pdiWheel_*.prx` stubs are never loaded here.)
2. **Crash `unknown sh reg at 00031: 0x0` (pm4Handlers.cpp ~2158).** GT7 sends *indirect register lists* of all-zero pairs
   (offset 0, value 0). The handler had no function for offset 0 and called `EXIT`. **Fix:** in `CpOpIndirectShRegs`,
   `CpOpIndirectUcRegs` and the CX variant, when `pfunc == nullptr` and `raw_cmd_offset == 0 && value == 0`, log once and
   `continue` (same pattern the author already used for CX `{0x24a,0}`). Effect on hardware unresolved — it works.
3. **Text rendered as dots; menus unreadable.** Three separate bugs, all in `src/libs/libFont.cpp` (the emulator's own font
   library, software-renders with stb_truetype):
   * a) GT7 measures each glyph with `sceFontSetScalePixel(10000)` and divides by 10000 to get size-independent metrics, but
     metrics were clamped to `FONT_BITMAP_MAX_DIM` (128) -> glyphs ~80x too small. **Fix:** `stb_metrics_pixel_height/scale`
     use the exact scale **only when `scale_h > 128`**; up to 128 the old behaviour is kept (see §7, Astro Bot).
   * b) `sceFontGetKerning` (NID `sDuhHGNhHvE`) was unimplemented (stub returns 0). **Fix:** implemented from the font's `kern`
     table via `stbtt_GetGlyphKernAdvance`, scaled by the metrics scale; codes with the top bit set are glyph indices;
     `prev_code == 0` = no kerning.
   * c) **THE spacing/missing-text bug:** the kerning result struct is **16 bytes** (`offset_x, offset_y, position_x, position_y`,
     as in upstream's reverted commit `f860c9c5`). The first implementation wrote only 8 bytes, so GT7 read **uninitialised stack**
     for `position_*` and laid text out with random gaps; some strings (the dialog body) vanished. **Fix:** `*kerning = {}` first,
     then set `offset_x`. LESSON: when a game reads a struct we only partly fill, zero the whole struct and compare sizes with
     upstream/SDK before theorising.
4. **GT7 draws text with MESH shaders**, one `vkCmdDrawMeshTasksEXT` per glyph, glyph bitmaps in separate 64xH R8 textures
   (cells), and does its OWN layout (zero calls to `sceFontCreateString`/`FontWriting*`). This is why a RenderDoc mesh-output
   dump (`rdoc_mesh.py`) was the key debugging tool.
5. **Dead ends (verified, don't retry):** em-size scale (`KYTY_FONT_EM_SCALE=1`, identical output), `image_metrics.bearing_x`
   (`KYTY_FONT_RESULT_MODE=1`/`2`, no change), per-font advance differences (3-6%), clamp of kerning. These switches remain in
   the code, default off.

---
## 6. Complete change list (all on branch `gt7-spike`)

| # | File | Change | Commit |
|---|---|---|---|
| 1 | `src/graphics/guest_gpu/command_processor/pm4Handlers.cpp` | skip all-zero indirect register pairs in SH/UC/CX handlers (log once) | `e6e1f12a` |
| 2 | `src/libs/libFont.cpp` | new `FontGetKerning` + register `LIB_FUNC("sDuhHGNhHvE", ...)` | `e6e1f12a`, `ada5ab05` |
| 3 | `src/libs/libFont.cpp` | `stb_metrics_pixel_height/scale`: exact metrics scale above 128 | `e6e1f12a` |
| 4 | `src/libs/libFont.cpp` | `KYTY_FONT_EM_SCALE` (em-size scale, default off), `KYTY_FONT_RESULT_MODE` (default 0), diagnostics `FontRender:` / `FontMetrics:` / `FontRenderMetrics:` log lines, `[SetScalePixel]`/`[SetupRenderScalePixel]` tags | `e6e1f12a`, `ada5ab05` |
| 5 | `src/libs/libFont.cpp` | kerning struct = 16 bytes, zero-initialised (**the spacing fix**) | `ada5ab05` |
| 6 | `src/graphics/host_gpu/renderer/pipeline/pipelineCache.cpp` (`InitializeProgramDiskCache`) | env `KYTY_PROGRAM_CACHE_MAX_MB` sets `settings.max_file_bytes` (default 512). GT7's translated shaders exceed 512 MiB; at the cap "new programs are no longer added" so they were re-translated every launch | **UNCOMMITTED** |
| 7 | `src/graphics/host_gpu/renderer/colorRenderTarget.cpp` (~line 324) | **EQAA support attempt:** was `EXIT` when `num_samples != num_fragments`; now `fragment_samples = render_sample_count(num_fragments)` (validity), host image `samples = render_sample_count(num_fragments)` (fragment rate = depth target's count; the first attempt used the coverage count and aborted with "mixed color/depth sample counts: 4 and 2"), `EXIT` only if either count is invalid. Guest layout code uses `num_fragments` too | **UNCOMMITTED, UNTESTED** |
| 8 | (scripts, not source) `work\play.ps1` got `-Game -Tag -EmuArgs`; new `gt7-build.bat`, `rdoc_*.py`, `gt7-uncommitted-2026-10-15.patch` | — | n/a |
| 9 | (scripts, not source) new `work\gt7fm.ps1` (fault-map + `--profile` launch in one line); `work\tracy_analyze.py` made header+encoding driven (UTF-16LE, 7-col csvexport) + per-source-line breakdown (§13 session 7) | — | n/a |
| 10 | `src/graphics/guest_gpu/hardwareContext.h` | diagnostic `Context::MemberNameAt` / `UserConfig::MemberNameAt` — `offsetof` tables that name the member containing a byte offset (for the draw-run key diff) | **UNCOMMITTED (diagnostic)** |
| 11 | `src/graphics/host_gpu/renderer/drawPrep/drawRun.cpp` | live switch **`KYTY_DRAW_RUN_KEY_DIFF`** (default off) + `NoteKeyDiff()`, called from `StructureKey`: reports which `Context`/`UserConfig` member differs from the previous draw's — the candidate for excluding from the run key | **UNCOMMITTED (diagnostic)** |
| 12 | `src/graphics/guest_gpu/command_processor/cpOps.h` | `LockstepReadOp` gained `buffer_copy` (0 = a plain CP read via `ReadGuestForCp`; 1 = `CheckCommandBytes` copying GPU-touched command bytes) | **UNCOMMITTED** |
| 13 | `src/graphics/guest_gpu/graphicsRun.cpp` | `CheckCommandBytes` sets `op.buffer_copy = 1`; `ExecOp` skips `DrawRun::NoteForeignActivity` for `LockstepRead` when `DrawRun::QuietLockstepEnabled()` and `buffer_copy == 0` | **UNCOMMITTED** |
| 14 | `src/graphics/host_gpu/renderer/drawPrep/drawRun.h` | declared `QuietLockstepEnabled()` (with the reasoning: a plain lockstep read only reads) | **UNCOMMITTED** |
| 15 | `src/graphics/host_gpu/renderer/drawPrep/drawRun.cpp` | live switch **`KYTY_DRAW_RUN_QUIET_LOCKSTEP`** (default off; the A/B lever) + `QuietLockstepEnabled()` | **UNCOMMITTED** |

Restore points (git tags): `restore-point-gt7-2026-10-15` (after #1-4), `restore-point-gt7-font-fixed-2026-10-15` (after #5).
Older tags: `restore-point-int191-2026-10-14` (Astro Bot daily-driver source, branch `int19.1-split`).
To discard experiments: `git checkout restore-point-gt7-font-fixed-2026-10-15 -- src` (do not run destructive git commands without Mark's OK).

---
## 7. Safety rule: do not break Astro Bot

* Upstream's own font commits (`f860c9c5` scaled kerning, `32e9fa30` glyph metrics/raster sizes) **broke Astro Bot's level titles**
  (wrong glyph sizes/spacing) and were reverted (`6acf3a6a`, `cb84831b`). Our metrics change is scoped to scale > 128 so Astro
  Bot (scale <= 128) is unchanged. **Before moving ANY of these changes into the Astro Bot exe, re-check Astro Bot level titles
  on screen** (enter a level; the level name appears).
* The Astro Bot build is `kyty_emulator.exe` (source branch `int19.1-split`/tag `restore-point-int191-2026-10-14`). Leave it.
* Upstream policy (README): PRs must be focused/game-specific; the HUMAN writes issue/PR text, not an AI agent. Nothing here is
  an upstream submission yet.

---
## 8. Performance findings (GT7)

Measured at the 3D tutorial (Le Mans/Alsace track):
* First launch / new scene: **0-1.3 fps**. Tracy (30 s): command-processor thread 95% busy; `PipelineCache::CreatePipeline(Gfx)`
  (really `GetGraphicsPipeline`, the lookup) = 17 s of 30 s, but **16,513 of 16,653 calls were instant; only ~30 real pipeline
  compiles at 100-730 ms each caused all of it**. Between compiles the game ran at 1,000-2,300 pipeline lookups/s with no stall.
* Pipeline caches work across launches (journal, driver cache `PPSA01317.bin`, known-pipelines), so each launch needs fewer
  compiles. In-run numbers: ~1,080 of 1,374 pipelines were cache hits on launch 2; ~295 new ones cost ~140 s total (~475 ms each).
* **Program cache cap** (change #6): the 512 MiB cap meant GT7 shaders past it were never saved. With
  `KYTY_PROGRAM_CACHE_MAX_MB=4096` the file grew to 693-734 MiB and stalls dropped.
* **Parallel compile:** the preset's `KYTY_PIPELINE_FAST_FIRST=1` turns prefetch off. Using `KYTY_PIPELINE_FAST_FIRST=0`,
  `KYTY_PIPELINE_PREFETCH=1`, `KYTY_PIPELINE_PREFETCH_THREADS=4` (max 4) compiled on 4 workers: CPU use rose from ~2.5 to ~4
  cores and fps from 0-1.3 to 3-12 (12 fps in the best stretch; 60 s average 17.6 fps once). Log line: `Pipeline prefetch: 4 compile workers`.
* **Write-fault storm:** in heavy scenes `Kyty fault cost` shows ~700-1100 faults/frame at 250-500 us each (200-390 ms/frame;
  "slow level 2"), 3,000 protect calls/frame, "BDA dirty log" ~300 MB/frame, ~10,000 draws/frame. Astro Bot by contrast: ~220
  faults at 56 us. This is the main remaining steady-state cost; knobs from the Astro Bot work (`KYTY_FAULT_AHEAD_*`,
  `KYTY_HOT_PAGE_*`) are documented in `AGENTSKYTY.md` §12-13 and were NOT tried on GT7 yet.
* GPU is nearly idle (1-20% at 210 MHz in stalls): this is a CPU-side problem, not GPU power.

---
## 9. GPU crash `ErrorDeviceLost` (not fixed)

* Seen once on GT7 (10:51) and three times on Astro Bot the day before. Upstream lists it as a known rare NVIDIA issue
  (release notes int19.1; workaround `KYTY_DEFER_PROTECTS=0`, costs ~2% fps; preset has `all`).
* **GT7's crash was decoded from the Windows System log** (Provider `nvlddmkm`, Event 13 + 153, no Kernel-Power 41/WHEA/BugCheck ->
  NOT a hardware freeze): `Graphics Exception on GPC 6: 3D WIDTH ZT Violation. Coordinates: (0x28, 0x0)`, `ESR ...=0x80000004`.
  A "WIDTH ZT violation" = the render area is wider than a bound depth/colour target (invalid Vulkan render area/attachment
  size) — an **emulator bug in render-state setup**, not a bad card. Read the raw text with:
  `Get-WinEvent -FilterHashtable @{LogName='System';ProviderName='nvlddmkm';StartTime=(Get-Date).AddMinutes(-40)} | Where Id -eq 13 | % { ($_.Properties | % Value) -join ' | ' }`
  (the plain `.Message` is empty).
* Code reviewed so far: `renderDraw.cpp` ~890-1100 computes `state.width/height` as the MIN over all colour attachments
  (`target.Extent()`, mip-aware) and the depth attachment (`depth.desc.info.extent`, mip-0; depth has no mip field). Looks right,
  so the cause is elsewhere (candidates: a draw whose viewport/scissor exceeds the attachment, a `renderArea` offset, mixed
  attachment sizes after EQAA, layered/array attachments).
* Next step to diagnose: run with `KYTY_DEVICE_FAULT_DIAGNOSTICS=1` (already in the run command above) and, when it recurs,
  read the `--- Device loss` block in `logs\<tag>.out` (fault addresses, vertex/pixel shader hashes of the draws in flight;
  `_device_fault.nv-gpudmp` if produced). Optionally `--vulkan-validation true` (very slow) to catch the invalid render area.

---
## 10. Known issues / next steps (do these in order)

1. **Verify change #7 (EQAA).** Run (§3). If it still dies, read the new `--- Error ---`/`Fatal` text in `logs\<tag>.out`; likely
   next messages: a depth-vs-colour sample-count mismatch, or an FMASK/CMASK path. File: `colorRenderTarget.cpp`, depth side
   `depthRenderTarget.cpp` (~153, 307 use `z.z_info.num_samples`). Vulkan needs equal sample counts in one pass unless a mixed-samples
   extension is enabled (NVIDIA has `VK_NV_framebuffer_mixed_samples`; not wired up here).
2. Commit #6/#7 once verified (Mark must ask), tag a restore point, update §6 and §13.
3. Reproduce/diagnose the ZT-violation crash (§9).
4. Performance: keep the cache-warming runs (each launch is faster); then try the Astro-Bot-era fault knobs on GT7
   (`KYTY_FAULT_AHEAD_ADAPT`, `KYTY_HOT_PAGE_MAX`), and re-profile with Tracy once compile stalls are gone.
5. Explore beyond the tutorial (main menu, a race) and note every new crash with its first `--- Error ---` line.
6. Unimplemented imports GT7 calls (all return 0, harmless so far): VrTracker2 x3, Usbd x2, Mbus, VideoOutVrrStatus, SystemService
   x2 (`9kPCz7Or+1Y`, `x1UB9bwDSOw`), NpSessionSignaling x2, TextToSpeech2 x2 (one is `sceTextToSpeech2Open`), RazorCpu x2.

---
## 11. Pitfalls that cost time (read before you run commands)

* **PowerShell 5.1:** `git commit -F -` with a here-string does NOT work; write the message to a file and use `git commit -F file`.
  `>` redirection re-encodes native-program output as **UTF-16LE** (cmd.exe `>` does not) — this silently broke the Tracy CSV analysis in session 7; `tracy_analyze.py` now sniffs the encoding. `&` is a PowerShell operator, invalid in cmd.exe (the Bash tool runs cmd).
  `&&` does not exist. `Start-Process -ArgumentList` does not quote: paths with spaces/brackets must be wrapped in `"` (play.ps1 does).
  `Remove-Item` on `logs\rdoc\*` is blocked by the harness; just overwrite files.
* **Windows file names cannot contain `:`** — RenderDoc ResourceIds look like `ResourceId::123`; sanitise them before saving (`SaveTexture` fails silently).
* **Foreground `Start-Sleep` > a few seconds is blocked by the tool harness**; put waits inside a script or use short sleeps.
* **Tracy/RenderDoc:** `--profile` needs the emulator init-order fix (already in this branch, commit `91b051ff`); `qrenderdoc` is a GUI that opens
  a window while a script runs (normal); if an "update available / cannot update while capturing" box appears it blocks scripts.
* **The emulator window does not respond while compiling** at 0 fps; closing may need `Stop-Process`. Caches are saved periodically, so a kill loses little.
* **Do not run two emulators** (shared `_kyty_input.txt`); a stale elevated instance cannot be killed from this non-elevated shell — ask Mark.
* **Keep `strata` closed**; check commit headroom (`play.ps1` prints it).
* Mark pastes whole transcripts back; give ONE command per code block when possible, and tell him exactly what to paste back.

---
## 12. How to keep this file current (do this after EVERY change or finding)

1. Add a dated line to §13 (what you did, result, next).
2. If source changed: update the table in §6 (file, what, commit hash or UNCOMMITTED) and refresh the patch:
   `cd C:\Users\mark\KytyPS5-src; git diff > C:\Users\mark\KytyPS5\work\gt7-uncommitted-<date>.patch`.
3. If a bug is fixed or ruled out: move it in §5/§10, keep the "dead ends" list honest.
4. If you learn a new command/tool/pitfall: add it to §3/§4/§11.
5. Put a one-line pointer in `AGENTSKYTY.md` top if the state changes materially.
6. Never delete evidence of a failed approach — record why it failed.

---
## 14. PLAYBOOK: bringing up ANY new game on this emulator (method that worked for GT7)

Rule of thumb: **one failure at a time. Never guess; read the evidence, change the smallest thing, rebuild, retest, record.**

1. **Check the compat DB / issue trackers first** (kytyps5emu.com/compatibility, KytyPS5 + Jetsku issues). Note the known error text; it tells you the first thing to expect.
2. **Find the dump** (`eboot.bin`, `sce_sys`, `param.json`, title ID). Make a NEW exe (`kyty_<game>.exe`) so the daily-driver exe is never overwritten. Use a new `-Tag` per run so logs are kept.
3. **Run with the preset + a guest log if text/audio is involved** (§3). Close `strata`. Read the END of `logs\<tag>.out`: the first `--- Error ---` line is the next thing to fix. Fix only that.
4. **Classify the failure and pick the tool:**
   * `EXIT`/`unsupported ...`/`unknown ... reg` in the log -> grep the message in `src\`, read ~30 lines around it, decide: is it a guard that is too strict (relax it), a feature never implemented (implement it), or genuinely impossible (stub + log once). Examples: GT7 zero register pairs (#1), EQAA sample counts (#7).
   * Wrong picture (black, garbled, missing text) -> **RenderDoc** (`--rd`, press F1 once, then the scripted analysis in §4). Work bit by bit: (a) does the frame have draws at all, (b) list textures and look at the PNGs for the broken element, (c) list the draws for that element, (d) dump their geometry/mesh outputs, (e) compare with what the game should have drawn. That is how GT7 text was traced to mesh-shader glyph quads and then to libFont metrics (§5).
   * Wrong values the game computes itself (layout, sizes) -> the game is reading something we return wrongly. Log the library calls it makes (guest log `FontMetrics:`-style diagnostics), compare struct sizes with upstream/SDK, zero-fill structs we only partly fill.
   * Slow -> **Tracy** (`--profile`, §4) to see which thread is the limit; read `Kyty fault cost` / `Pipeline` lines; compile stalls vs write-fault storm need different fixes (§8).
   * GPU crash (`ErrorDeviceLost`) -> Windows System log `nvlddmkm` event text names the cause (§9); run with `KYTY_DEVICE_FAULT_DIAGNOSTICS=1`.
   * Unknown imported function -> compute its NID from candidate names (§4) before implementing.
5. **Change, rebuild (§2), retest the SAME scenario.** A fix can expose the next, different error a few seconds later (GT7: EQAA `samples=2 fragments=1` -> then `mixed color/depth 4 and 2`). That is progress, record both.
6. **Verify assumptions with a number from the log**, not intuition (the first EQAA fix guessed "coverage count"; the log said depth was 2, so it had to be the fragment count).
7. **Protect the other games:** scope changes so existing behaviour is unchanged (e.g. only above a threshold), and re-check Astro Bot (§7) before moving any change into its exe.
8. **Record as you go (§12):** dated line in §13, update the §6 table, refresh the patch file, keep failed approaches and why. Commit only when Mark asks; tag restore points.
9. **First-run waits are normal:** black screen / 0 fps for minutes on a new scene = shader translation + pipeline compile with CPU 2-4 cores busy and the log still updating. Real hang = log silent, no CPU. Caches make later runs faster; do not kill it.
10. **Mark's workflow:** he runs the slow game parts and pastes results; give ONE command per block and say what to paste back. Screenshot with `capture.ps1` to see state yourself.

---
## 13. Session log (newest last)

* **2026-10-15 (session 5, GT7).** Found the dump; compat DB says "doesn't boot". Built branch `gt7-spike`. Fixed zero-register-pair EXITs (#1).
  Fixed font rendering (#2-#5): scale clamp, kerning stub, kerning struct size (the spacing bug). Menus now fully readable, 3D tutorial
  renders. Profiled with Tracy (compile stalls) and found the 512 MiB program-cache cap (#6) and the benefit of parallel prefetch.
  Decoded an `ErrorDeviceLost` as an NVIDIA "3D WIDTH ZT Violation" (§9). A later crash `unsupported render-target sample configuration:
  samples=2 fragments=1` (EQAA) led to change #7 — **built and queued for testing by Mark, result unknown at time of writing.**
  Commits `e6e1f12a`, `ada5ab05`; uncommitted #6/#7 saved in `gt7-uncommitted-2026-10-15.patch`.
* **2026-10-15 (session 6, GT7).** Run `gt7t` tested #7: got past the old EQAA EXIT, ran ~42 fps in the tutorial (write-fault
  cost only 8 ms/frame, slow level 0, program cache working), then died right after the "time to put the pedal to the metal"
  line with `mixed color/depth sample counts are unsupported: 4 and 2` (`pipelineCache.cpp:6877`). Cause: #7 gave the host colour
  image the COVERAGE count (4) but the depth target is stored at FRAGMENT rate (2). **Revised #7:** `colorRenderTarget.cpp`
  host image `samples = render_sample_count(num_fragments)` (extra coverage samples dropped); EXIT only if either count is
  invalid. Patch refreshed; rebuilt as `kyty_gt7.exe`. **Run `gt7u` with the revised #7: PASSED the old crash point** — the
  "All right, time to put pedal to the metal!" dialog rendered (Alsace Village track, 3-9 fps), Mark pressed Start, then a
  black 0 fps screen while the race scene's first-time shaders compiled (CS with BVH ray-tracing intersections, software RT;
  process alive, ~2.3 cores, log updating). **Mark then DROVE the car in the tutorial: it works, but extremely slowly (~1 fps at first entry while shaders compile).** First time GT7 is actually driveable. Added §14 (general playbook).
* **2026-10-15 (session 6, run `gt7v`, warm caches).** Menus/tutorial 41-58 fps (1-92 faults/frame, slow level 0). Race scene
  (Alsace Village) ~6-11 fps: `1429 faults/frame at 128 us = 183 ms/frame`, 2644 protect + 1599 unprotect calls/frame, slow
  level 0. vs cold run `gt7u`: 1312 faults at 326 us, 1.5 fps. So warm caches halved per-fault cost, compile stalls are mostly gone,
  and the **fault COUNT (~1,400 pages written/frame) is now the limiter**. A 0 fps stall happened when the car crashed (new
  effects/shaders). Next experiments (not yet tried on GT7): `KYTY_FAULT_AHEAD_ADAPT` (auto/512/1024), `KYTY_HOT_PAGE_MAX`,
  `KYTY_HOT_PAGE_FRAMES`, `KYTY_TEXTURE_FAULT_FAST_PATH=1`; then Tracy.
* **2026-10-15 (run `gt7w`): DEAD END.** `KYTY_TEXTURE_FAULT_FAST_PATH=1` + `KYTY_HOT_PAGE_MAX=16384` in the race scene: 6.3 fps,
  776 faults/frame at 269 us = 209 ms/frame, slow level 2 (vs defaults `gt7v`: 6.2 fps, 1429 faults at 128 us = 183 ms, level 0).
  Fewer faults but each costs ~2x -> no fps gain (same as on Astro Bot). Do not retry these knobs. Next: Tracy profile of the race
  scene to see where the other ~half of the frame goes, and look at why GT7 touches ~1,400 pages/frame. Earlier: let caches warm (relaunch, drive again), then profile with Tracy; commit #6/#7 when Mark asks.
* **2026-10-15 (session 7, CommandCode).** Ran START HERE step 1: fault map + Tracy on the race scene (run `gt7fm1`, launch helper `work\gt7fm.ps1`; the fault map + `--profile` in one driving session). Tooling fixed: PowerShell 5.1 `>` redirection writes **UTF-16LE**, which made `tracy_analyze.py` parse 0 rows; it now sniffs the BOM, drives columns from the header (the csvexport 0.14.1 layout is 7 columns: `name,src_file,src_line,ns_since_start,exec_time_ns,thread,value`), and prints a per-source-line breakdown.
  * **Fault map** (`KYTY_FAULT_MAP=1`, `logs\gt7fm1.out`): race scene **1,070–1,300 faults/frame at 6–8 fps**; the faults are **guest threads, not the CP** (e.g. `guest 1168.7, cp 130.9`), spread over six job threads (`Job#0..#5`, ~150 each). Guest fault = **133 µs**, of which **84 µs is `flush`** = the GPU **readback** in `BufferCache::InvalidateMemory` -> `ReadMemory(vaddr, size, true)` (`bufferCache.cpp:1436-1444`): the guest is writing pages the GPU owns. `same page faulted again same frame 535/frame`; `page already CPU-dirty 196/frame`. Top regions: `0x143cc00000` (thread `PCL Event`, ~355 µs/fault) and a wide band `0x1000000000`–`0x1005800000` (~50–60 faults/frame each). Re-protection is huge: `protect host read-only 1728 calls / 37936 pages/frame`, `protect guest read-write (in fault) 1353 calls / 35515 pages/frame`.
  * **Tracy** (`logs\tracy_gt7fm1.tracy`, 30 s, ~8 fps during the capture, 13.0 M zones; `logs\tracy_gt7fm1_threads.txt`): **thread 1 (CommandProcessor) is 99 % busy**, DrawPrep workers (threads 3–6) ~0–1 %, present thread 7 %. `Presenter::PrepareFrame` = 250 calls / 30 s => **~8.3 fps => ~119 ms/frame on thread 1 alone**. Split: `CommandProcessor::Process` self **39 ms/frame** (`graphicsRun.cpp:1687`), `CommandProcessor::Resolve` self **33 ms/frame** (`graphicsRun.cpp:5231`, which contains the `_mm_pause` **starvation spin** at 5266-5279), then the draw commit **~46 ms/frame** (`DrawIndex` 11 / `renderDraw.cpp:4166` at 6.5 µs/draw, `RebindBuffers` 9.3 / `descriptors.cpp:2015`, `ResolveRenderColorTarget` 5.3 / `colorRenderTarget.cpp:267`, `CommitBindings` 4.6, `PrepareBindings` 3.9, `DrawAuto` 3.7, `CreatePipeline(Gfx)` 2.8). **The CP's own Process+Resolve loops (72 ms) cost more than the actual draw commit (46 ms).**
  * **Fault path exonerated again for GT7:** the guest faults are large in wall-clock terms (137 ms/frame summed over 6 threads) but they do not limit — each guest thread is only ~19 % busy; thread 1 is the pole. (Tracy + fault map together inflate the run: fps 8 -> 5-9 while profiling.)
  * **Next:** (a) quantify starved-vs-work headlessly with `KYTY_CP_COMMIT_STATS` (`CommitStats ... gapsplit[...]` per draw class, `CpGaps` ledger; `drawPrep/commitStats.cpp`) — no Tracy needed; (b) finer zones inside `Process`/`Resolve` self; (c) the new GT7-specific lead: the **84 µs GPU readback per guest write** (why does GT7 rewrite GPU-owned pages ~1,000x/frame?).
* **2026-10-15 (session 7, CP gap split DONE — run `gt7cp3`).** `.\gt7fm.ps1 -Tag gt7cp3 -NoFaultMap -NoProfile -CpStats` (no fault map / no Tracy, so the guest producers are not slowed). **Race window at 6.3 fps (63 frames/10 s, 3,596 draws/frame): commit ≈ 58 ms + gap ≈ 93 ms ≈ 151 ms of a 158.7 ms frame** — the CP thread still accounts for the whole frame. Per draw class (draws/frame × [commit + gap] us/draw):
  * **instance 388/f × [72.4 + 137.5] = 81.5 ms (51 %)** — by far the worst; 15x the cost of a `cont` draw. Its gap is `gpuwait.drain 62.9` + `fault 11.6` + `protect 9.4` + `recorder 8.5` + `headwait.barrier 8.4`.
  * **runmiss 1,700/f × [9.1 + 10.4] = 33.1 ms (21 %)** — nearly half of all draws; gap is almost all `headwait.barrier 9.1`.
  * **pipeline 696/f × [13.9 + 22.3] = 25.2 ms (16 %)** — gap `headwait.barrier 19.0`.
  * structure 458/f = 7.2 ms; `first` 1/f = 4.2 ms (inter-frame, `idle` only 76 us — the frame-capped menus idle is gone); dynamic 3.3/f = 0.06 ms; **cont only 11/f ≈ 0**.
  * **The gap (93 ms) is bigger than the commit (58 ms), and it is dominated by waiting on the GPU**: `headwait.barrier` ≈ 34 ms/frame + `gpuwait.drain` ≈ 24 ms/frame. Front-end starvation is NOT the story: `starved` totals ≈ 2 ms/frame (I had suspected the `_mm_pause` spin — it is small). `fault` ≈ 4.5 ms + `protect` ≈ 3.7 ms are the guest write-tracking's share on the CP.
  * **GT7 never continues a draw run** (`cont 11/f`; `runs ... 1:27728/27728` — every run has length 1, `cont same-frame/all 0 %`). So DrawRun's within-run delta commit does nothing for GT7: every draw pays the full ~9-72 us commit. `start why` = structure ~70 %, instance ~15 %, pipeline ~13 %.
  * **So the next target is the barrier/drain waits and the `instance` class**, not the starvation spin. Options to investigate: why each draw needs a barrier (`headwait.barrier`), what `gpuwait.drain` waits on (upload DMA / submit fence), and the 72 us commit of `instance` draws (phases for that class are in the same `StartPhases` line, `logs\gt7cp3.out:1485`).
  * Follow-up on the menus (`gapsplit` at 60 fps): `idle ≈ 14.9 ms/draw` — confirms the CP is only saturated in the race.
* **2026-10-15 (session 7 cont.) — authoritative per-frame split + the two mechanisms behind it.** The **`CpGaps`** line (printed every 10 s under `KYTY_CP_GAP_STATS`, one per guest frame boundary) gives ms/frame directly, so it beats the per-class `gapsplit`. Race window: `wall 157.11 ms/frame | commit 63.62 (3614/f) | gap 93.49 = headwait.barrier 34.471 (3074.6/f) gpuwait.drain 24.340 (15.5/f) fault 4.586 (149.5/f) protect 4.067 recorder 3.873 service 3.641 starved 3.194 disp.rebind 2.333 prog.materialize 1.417 resolver 1.257 slice 1.240 ...` (everything else < 1.2 ms).
  * **`headwait.barrier` (34.5 ms/frame) = the CP waiting for the DrawPrep worker that claimed the head slot** — `drawPrep.cpp:1393-1458` (`ReadyHead`), ~3,075 waits/frame ≈ **one per draw, 11.2 µs each**. The category comes from `CpGaps::Relabel(slot.after_stop == 1 -> HeadWait/HeadWaitBarrier)`; `after_stop == 1` is raised by `DrawPrep::Engine::NoteStop(1)` in the **`WAIT_REG_MEM` lockstep handler** (`graphicsRun.cpp:4566`) — GT7 issues ~3,075 lockstep reads/frame, one before almost every draw (the same `LockstepRead` reason that dominates `DrawRun`'s misses). `after_stop` only *relabels* an existing hand-off wait; the wait itself is the CP spinning for the worker (`KYTY_DRAW_PREP_STEAL`, which can steal those, defaults off).
  * **`gpuwait.drain` (24.3 ms/frame) = only 15.5 waits but 1.57 ms each** — the CP blocked in `BufferCache::ReadMemoryDrain` -> `m_scheduler.Wait(tick)` (`bufferCache.cpp:1817-1849`): waiting for a **GPU readback** to finish. The line also prints `drains/f: write 4.19 current 9.19 unbounded 0.00 other 0.02 cp-thread 2.41 side-flushes 0.28 side-flush-copies 0.28`.
  * **Live switches available for same-process A/B** (`KYTY_LIVE_FILE=<path>`; write `KEY=VALUE` lines, applied on the CP thread at the next guest flip, logged `Live: seq N applied at CP flip ... (cp_busy_us ...)`; unmentioned keys keep their value; `KEY=` resets to default): `KYTY_READBACK_FLUSH_SIDE` (default off), `KYTY_READBACK_SIDE_WRITES` (on), `KYTY_READBACK_MERGE_GAP_KB`, `KYTY_DRAW_PREP_DISPATCH` (off), `KYTY_DRAW_PREP_DIRECT_READS`, `KYTY_DRAW_PREP_CLEAN_SINCE`. **Not live** (startup only): `KYTY_DRAW_PREP_STEAL`(0)/`KYTY_DRAW_PREP_STEAL_AFTER_US`(0), `KYTY_DRAW_PREP_WORKERS`(6)/`_WINDOW`, `KYTY_READBACK_SIDE_GPU_THREAD`(1), `KYTY_READBACK_EAGER`(1), `KYTY_WRITTEN_SYNC_SKIP`(1), `KYTY_FALSE_SHARING_WRITES`.
* **2026-10-15 (session 7 cont.2) — live-switch A/B run `gt7ab` (KYTY_LIVE_FILE) + a device loss.** One launch, in the race:
  `.\gt7fm.ps1 -Tag gt7ab -NoFaultMap -NoProfile -CpStats -Live` (adds `KYTY_LIVE_FILE` -> `work\live.txt`; the agent rewrites that file
  between readings). Live control works end-to-end: `Live: seq 1 applied at CP flip 5632 (t_ms 180757, cp_busy_us 93771520): KYTY_READBACK_FLUSH_SIDE=1 (0 -> 1)`.
  * **`KYTY_READBACK_FLUSH_SIDE=1` — promising, but UNVERIFIED (see the crash below).** It serves the *current*-data GPU readbacks as **side copies**
    instead of blocking drains: `drains/f: current 9.19 -> 0.00`, `side-flushes 0.28 -> 6.4-8.7`. In the same scene it cut **`gpuwait.drain` ~58 -> ~5-13 ms/frame**
    and **wall ~199-231 -> ~107-171 ms/frame** (title fps ~5 -> 7-9); `headwait.barrier` also fell (39-53 -> 16-35). No correctness warnings
    (`verify 0 checks, 0 mismatches`, no `lost_pages`). `fault` rose (~4.6 -> ~20 ms/frame) because the guest side now pays for the side copies — net still a large win.
  * **Then it died in the KNOWN `ErrorDeviceLost`** (§9): `--- Device loss: submission/wait tick=248785`; `submit upload DMA copies failed:
    ErrorDeviceLost (-4)` at `vulkanCommon.cpp:142`; device fault `WriteInvalid at 0x0` (`_device_fault.nv-gpudmp`); `vkQueueSubmit ... ErrorDeviceLost`;
    Windows System log `nvlddmkm` id 13 `Graphics Exception on GPC 0: 3D WIDTH ZT Violation. Coordinates: (0x88, 0x8)` + id 153 at **14:20:30** — the same
    signature as §9 (5th reset today). It died **before my revert applied** — only `Live: seq 1` appears in the log, so FLUSH_SIDE was still **1** at the crash.
    **Verdict: cannot clear it and cannot blame it.** Re-test in a fresh run; if the device loss recurs with FLUSH_SIDE=1 and not with it off, treat the switch as a suspect.
  * Extra observation from this run's `Steps` data: the **`instance` class pays `markbindings` ~30-40 us/draw** (39.965 / 39.191 / 30.481) — a concrete
    per-draw cost worth attacking alongside the barrier wait.
  * GPU state after the crash: card recovered (driver reset, `nvlddmkm` 153), no Kernel-Power 41 — same "recovered reset, not the old hard freeze" pattern as §9.
* **2026-10-15 (session 7 cont.3) — `gt7ab2`: `FLUSH_SIDE` is NOT an fps win (matched-load A/B); the crash did not reproduce.** Run
  `.\gt7fm.ps1 -Tag gt7ab2 -NoFaultMap -NoProfile -CpStats -Live` (launched by the agent, PID 44388). Live sequence, all applied and logged:
  baseline 0 -> `seq 10` =1 -> `seq 11` =0 -> `seq 12` =1.
  * **Mechanism verified and reversible:** with `FLUSH_SIDE=1` the `current` drains go to **0.00/frame** and `side-flush-copies` to 6.8-9.6/frame;
    `seq 11` (=0) restores `current` 4.9-6.3 and `side` ~0.1. So the switch does exactly what it says.
  * **No fps gain at matched load.** Matched windows (same frames/10 s): **off** `L315 fr56 wall 180.82 drain 26.46` vs **on** `L385 fr56
    wall 179.88 drain 6.05` — the ~20 ms of CP drain wait is simply re-attributed (commit 56.9 -> 117.9 while gap 123.9 -> 62.0 at the *same*
    wall), not removed from the critical path. **The drain wait is not the frame limiter** (same lesson as the Astro Bot fault path, `AGENTSKYTY.md` §13).
  * **The `gt7ab` "win" was scene drift** — its baseline windows happened to be unusually heavy (`drain 58 ms`, wall 199-231). **Do not adopt `FLUSH_SIDE`.**
  * **The device loss did NOT reproduce** over ~4 min of driving with the switch both on and off (emulator alive at the end, frame 4079). The `gt7ab`
    crash was therefore almost certainly the known §9 issue, not the switch.
  * **Method note (important):** the race scene's load drifts a lot (wall 83-218 ms across windows), so only compare windows with the **same
    `frames/10 s`**; the `Live: seq N applied at CP flip ...` lines are the only reliable alignment points.
  * Consequence: the CP's *waits* (drain, and probably `headwait.barrier`) are not simple limiters — they overlap with the pipeline's other work.
    The frame is the CP's total time; attacking it needs either less CP work per draw (the `instance` class: `markbindings` ~30-40 us/draw) or
    fewer draws, not fewer waits.
* **2026-10-15 (session 7 cont.4) — next lever: why GT7 never continues a draw run. Instrumented.** `DrawRun::StructureKey`
  (`drawRun.cpp`) hashes the **whole `HW::Context` and `HW::UserConfig`** plus programs/T#-S#. GT7's runs are all length 1 (`cont 11/frame`,
  `start why: structure ~70 %`), so ~3,600-4,700 draws/frame pay the full ~9-87 us commit instead of the 3.5 us a `cont` draw costs. Any register
  in those structs that changes every draw (without affecting the reused structure) would explain it.
  * Added a **switch-gated diagnostic**: `hardwareContext.h` gained `Context::MemberNameAt`/`UserConfig::MemberNameAt` (`offsetof` tables that
    name the member holding a byte offset) and `drawRun.cpp` gained the live switch **`KYTY_DRAW_RUN_KEY_DIFF`** + `NoteKeyDiff()`, called from
    `StructureKey`, which prints `DrawRunKeyDiff #N: context <member> (byte X), config <member> (byte Y)` for each draw whose context/config
    differs from the previous one (first 32 then every 512), plus an 8192-draw summary. Changes #10/#11 in §6. **Rebuild required (done via
    `work\gt7-build.bat`).**
  * The diagnostic answers at the **menus** too (the menus show the same pathology: `start 97.8 %`, `start why: structure 70 %`), so no driving
    is needed for it — one mouse click (the PSN dialog) plus `_kyty_input.txt` key sends is enough.
  * If the culprit is a spurious register, the fix is to exclude it from the key (and validate with `KYTY_DRAW_RUN=verify`, which compares the
    skipped work against the normal path and counts mismatches) — that could move most of 3,600-4,700 draws/frame from tens of microseconds to 3.5 us.
    If it is a legitimate per-draw field, stop and pivot (upstream is separately building cross-frame draw caching).
* **2026-10-15 (session 7 cont.5) — run-key hypothesis REFUTED; the real breaker is the per-draw lockstep read.** Diagnostic run `gt7diag`
  (agent-launched `.\gt7fm.ps1 -Tag gt7diag -NoFaultMap -NoProfile -CpStats -Live -Extra 'KYTY_DRAW_RUN_KEY_DIFF=1'`, welcome-to-GT7 3D background):
  * `DrawRun 10s (on): 318699 draws, 90845 eligible, 70694 key matches, 2282 continued (0.7%)`; `misses: activity=68386 command=26 images=1
    (activity by service command=58 LockstepRead=68312 WaitRegMem=16)`. **The key matches 78 % of eligible draws — the key is NOT the problem.
    96.8 % of key matches die on the activity epoch, and 68,312 of those are `LockstepRead`.**
  * The key diff agrees: `context same` in ~85-88 % of consecutive draws (the remainder differ in `m_sh_regs` etc. — legitimate per-draw state), and
    the config differs in 96 % but **only in `m_object_id`, which the key already zeroes**. So the key is doing its job; the exclusion idea is dead.
  * **Root cause (shared with `headwait.barrier`):** GT7 issues a `WAIT_REG_MEM` / lockstep read before nearly every draw. `ExecOp`
    (`graphicsRun.cpp:4151-4158`) calls `DrawRun::NoteForeignActivity(CpSeq::OpKindName(kind))` for every op kind except
    DrawIndex/DrawAuto/ReadCheck/CondExec/Branch/quiet-WriteData/prepared-indirect — **`LockstepRead` is not excluded**, so each one ends the run;
    the same op raises `NoteStop(1)` (`graphicsRun.cpp:4566`), which is what labels the next draw's head wait as `headwait.barrier`.
  * **Candidate fix (careful, needs verify):** treat `LockstepRead` as non-activity, like the author already does for `WriteData` / synthetic
    `EventWrite` (`KYTY_DRAW_RUN_QUIET_*`, `drawRun.h`). **The catch:** there are two lockstep reads — plain guest reads
    (`ExecLockstepRead` -> `ReadGuestForCp`) and `CheckCommandBytes` copying GPU-touched **command bytes** (`graphicsRun.cpp:4630-4667`), which
    genuinely changes what will execute. Only the plain-read kind may be quietened. Validate with `KYTY_DRAW_RUN=verify` (must stay 0 mismatches).
  * **Implemented (changes #12-#15 in §6):** `LockstepReadOp` gained `buffer_copy` (0 plain read, 1 `CheckCommandBytes`), `ExecOp` skips
    `NoteForeignActivity` for `LockstepRead` when the new **live switch `KYTY_DRAW_RUN_QUIET_LOCKSTEP`** (default off) is on and `buffer_copy == 0`.
    Default off = no behaviour change until A/B'd.
  * **A/B RESULT (run `gt7q`, 3D scene, live `Live: seq 20 applied ... KYTY_DRAW_RUN_QUIET_LOCKSTEP=1`): mechanically confirmed, but no fps win.**
    Continuations went **4.0-14.9 % -> 17.9-25.4 %** (e.g. `300451 draws, 114224 eligible, 85788 key matches, 76363 continued (25.4 %)`) and the
    activity misses collapsed (`activity=8335`, `LockstepRead=6547`; was `activity=68386`, `LockstepRead=68312`) — so the diagnosis is exactly right.
    **However `wall` did not improve** (matched: `fr72 wall 140.13 ms` off vs `fr70 wall 144.54 ms` on) and **`headwait.barrier` rose** (12.6-22.0 ->
    24.7-53.9 ms). Same lesson again: moving work off one place does not move the frame here.
  * **Still required before any adoption:** (a) re-test in the **race** scene (this was the lighter 3D background), (b) run the safety check
    **`KYTY_DRAW_RUN=verify`** (live-settable) and require **0 mismatches** — the switch relaxes a correctness certificate, so it must not be
    trusted on the strength of a speed test alone. Switch reverted to 0 afterwards (`Live: seq 21`).
  * **SAFETY CHECK PASSED (live `seq 22`: `KYTY_DRAW_RUN=verify` + `KYTY_DRAW_RUN_QUIET_LOCKSTEP=1`):** `DrawRun 10s (verify): ... verify 98878 checks,
    0 mismatches`, then `139463 checks, 0 mismatches`, `90275 checks, 0 mismatches` — **~330,000 compared draws, 0 mismatches**, i.e. the relaxed
    certificate produces no difference from the normal path in this scene (the author's own acceptance bar for the other quiet switches). Activity
    misses under verify fell to `activity=6881` / `3862` (from 68,386). So the change is *mechanically correct* by this measure; only its **speed
    effect in the race** is unproven.
  * **Net so far:** continuations +verified, correctness +verified, fps — **not yet demonstrated**. Next: measure in the race scene.
* **2026-10-15 (session 7 cont.9) — race sweep INCONCLUSIVE: one-shot A/B is unusable because the race load drifts.** Run `gt7r1`, in the race,
  phases 34 s each: baseline -> `QUIET_LOCKSTEP=1` -> 0 -> `READBACK_MERGE_GAP_KB=64` -> 0.
  * baseline `wall` 148-174 ms / 5.9-6.4 fps, `headwait.barrier` 23-27 ms; `QUIET_LOCKSTEP=1` 123-154 ms / 6.5-9.1 fps, `headwait.barrier` **15-22 ms**,
    continuations **0.3 % -> 14.4 %**; back to 0: 104-162 ms / 0.2 %; `MERGE_GAP_KB=64`: 140-230 ms (rising) / 1.0 %, `headwait.barrier` 42-49.
  * **The level rose monotonically through the sweep (approx 160 -> 256 ms) as the car moved**, so the phases are not comparable and **neither
    effect is proven**. `QUIET_LOCKSTEP` still looks mildly positive; `MERGE_GAP_KB` looks worse.
  * **METHOD FIX (do this next):** alternate the switch every ~10-12 s (**A/B/A/B/A/B… 8+ samples each**) and compare the two groups; ideally with the
    car **stationary in the race** (a parked car still renders the full scene) so the load is constant. Single 30 s phases are worthless here.
  * **Run `gt7q` then ended in the known `ErrorDeviceLost`** (14:49:00): `--- Device loss: submission/wait tick=396617 ---`,
    `vkQueueSubmit ... ErrorDeviceLost (-4)` (`queueSubmission.cpp:474`), device fault `WriteInvalid at 0x00000004abd50000`;
    Windows `nvlddmkm` 153. It died **after** everything had been reverted to defaults (`Live: seq 23` = `DRAW_RUN=1`, `QUIET_LOCKSTEP=0`), so it does
    **not** implicate the new switch. It does mean the **race-scene fps test for `KYTY_DRAW_RUN_QUIET_LOCKSTEP` is still outstanding**. (Two resets seen
    today: 14:20:30 and 14:49:00; both driver-recovered, no Kernel-Power 41 — the §9 pattern.)
  * **Scripted game control (agent attempt):** `focus.ps1` works, and new **`work\click.ps1`** (SetCursorPos + `mouse_event` left click at absolute
    screen coordinates) runs, but a synthetic click at GT7's PlayStation Network dialog **Cancel** button (window rect + image-relative coords:
    screen 3179,868 for a window at 2532,434, image 647,434) **did not activate it** — the dialog stayed up while the frame counter advanced.
    So that specific dialog still needs a real click, or `click.ps1` needs DPI/`SendInput` work. `key.ps1` (file input) remains the tool for
    everything that accepts the controller.
* **2026-10-15 (session 7 cont.6) — save data: diagnosis of "GT7 forgets settings".** Explored `src/libs/libSaveData.cpp` (the whole 1354-line
  library) + `src/kernel/fileSystem.cpp`:
  * **The mount layer is genuinely disk-backed, and the no-op `SaveDataCommit`/`SaveDataPrepare` stubs lose nothing** — guest writes go straight to
    the host directory through `KernelWrite` -> `MountPoints::ResolvePath` (`fileSystem.cpp:257-404, 679-742`). Stubs that *do* discard content:
    `SaveDataSaveIcon`/`SaveDataSaveIconByPath`/`SaveDataLoadIcon` (icons), `SaveDataBackup`, `SaveDataDeleteTransactionResource`. **None of GT7's
    save imports is unresolved.**
  * **Layout:** the root is the **CWD-relative** `_SaveData` (`libSaveData.cpp:29`):
    `<cwd>\_SaveData\<TITLE_ID>\<dir>\sce_sys\{param.bin,blocks.bin}` and `<cwd>\_SaveData\<TITLE_ID>\sce_sdmemory\<user>\memory.dat`.
    `play.ps1` sets the cwd to `KytyPS5-jetsku`, so GT7's tree is `C:\Users\mark\KytyPS5-jetsku\_SaveData\PPSA01317\` (today only
    `sce_sdmemory\1000\memory.dat` plus `LIVERYIV`/`DRFILEIV` with `sce_sys` and **no data files**). A second tree exists under `KytyPS5\work\`
    (Astro Bot's) — the path is cwd-dependent, so a launch from a different cwd reads a different save.
  * **The evidence says GT7 never runs its save, rather than the emulator losing it:** GT7 sets up a 32 MB `sce_sdmemory`
    (`SaveDataSetupSaveDataMemory2(..., memory_size=33554432, slot 0)`, `gt7j_guest.txt:730453`) and reads it repeatedly, but **no
    `SaveDataSetSaveDataMemory2` / `SaveDataSyncSaveDataMemory` call appears in any captured guest log**, and the mount dirs hold `sce_sys` only.
    So the settings were never written (and a forced `Stop-Process` would lose them anyway).
  * **Next test:** make GT7 save deliberately (change settings so it saves, or exit to a menu cleanly), then watch the mtime of
    `_SaveData\PPSA01317\sce_sdmemory\1000\memory.dat`, and grep a guest log (`--printf-direction File`) for `SetSaveDataMemory`/`SyncSaveDataMemory`.
* **2026-10-15 (session 7 cont.10) — AGENT GAME CONTROL SOLVED: I can drive GT7 myself now.** New helper **`work\gt7ctl.ps1`**
  (`-Steps 'focus','winkey Escape','wait 2','tap J','hold W 1.5','shot logs\x.png'` — see §4). What each channel does:
  * **Pad buttons via the `_kyty_input.txt` file channel need NO focus** — `ApplyHostKey` (`hostInput.cpp:452-460`) has no focus guard, it maps
    the SDL key straight onto a controller control. **Verified end-to-end:** `key J down` / `key J up` dismissed GT7's own
    "Failed — You cannot use network functions…" dialog and advanced the game into its intro cinematic; the file drains to 0 bytes, so the
    polling is live. **This is the main channel — it covers dialogs, menus and driving.**
  * **The emulator's OWN dialogs ignore the pad map, but take posted window keys:** `PostMessage WM_KEYDOWN/KEYUP` with **Escape (0x1B)** or
    Return (0x0D) to the `SDL_app` window dismissed the PSN "sign-in is unavailable" dialog. No focus needed.
  * **Injected mouse does NOT work** (so `click.ps1`/`click2.ps1` are dead ends for the emulator UI): `SetCursorPos` lands exactly
    (asked 1919,819 -> read back identical; desktop 3840x1600; window rect 1272,385..2568,1144, so **not** a DPI problem) and both `mouse_event`
    and `PostMessage(WM_LBUTTONDOWN/UP)` do reach the window (the Cancel button visibly took a hover state) — but the click is never honoured.
    `mouse_event` does need the window to be genuinely foreground (`GetForegroundWindow` was the tool's console first; `SetForegroundWindow`
    fixed that) — but even then the click was ignored. The process has exactly one visible top-level window (`SDL_app`), so the dialog is drawn
    in-window, not a native MessageBox.
  * **If a mouse-only UI ever blocks a route:** add a `mouse <x> <y> down|up` (or `click <x> <y>`) command to the file-input parser in
    `hostInput.cpp` (`HostInputPollCommandFile`, ~line 92-122) so the click is synthesised in-process like the pad keys — focus-free and reliable.
  * **Result:** from a cold launch I can now reach the game's menus/3D unaided (proved: PSN dialog -> GT7 dialog -> intro cinematic, all scripted).
* **2026-10-15 (session 7 cont.11) — AGENT CONTROL FINISHED: the real cause was `tap`.** Verified with the emulator's own trace
  (`KYTY_HOST_INPUT_TRACE=<csv>` -> `ms,event,a,b,c` lines):
  * **`tap <KEY>` is useless for game input.** The file channel calls `ApplyHostKey` **directly** (`hostInput.cpp:107` and `:112-113`), bypassing
    the input pulse (`g_input_pulse`, which is what `KYTY_HOST_INPUT_MIN_PRESS_MS` tunes and which only the SDL event path uses — `HostInputKey`,
    `hostInput.cpp:556-559`). So `tap J` emits `key J down` + `key J up` **in the same millisecond** — the trace shows
    `apply-key,106,1` and `apply-key,106,0` at the identical timestamp — and a 60 Hz game never samples it. **Always use an explicit
    `key <K> down` / wait / `key <K> up` pair** with >=1 frame (>=150 ms) between them.
  * **Proved working:** `down J` -> `wait 1` -> `up J` (trace: `apply-key,106,1` at 27575997, `apply-key,106,0` at 27577026 = a 1029 ms hold)
    **dismissed GT7's "Failed / You cannot use network functions" dialog** and advanced into the intro cinematic.
  * **`winkey Return` dismisses the emulator's own PSN "sign-in is unavailable" dialog** (Escape alone did not; Return is its default button).
    That is the *emulator's* dialog, not the game's, so the pad map never applies to it.
  * `focus-lost` in the trace = the SDL window losing focus, which calls `g_input_pulse.ReleaseAll` (`hostInput.cpp:638-644`) — so a **held** key
    is released if focus is lost mid-hold; discrete presses are unaffected. `focus` before a hold, or keep holds short.
  * `KYTY_HOST_INPUT_ONLY=1` makes `GameController::Connect` ignore every non-synthetic pad (`controller.cpp:443`), so the game can only see the
    scripted pad — useful when Mark's physical Xbox pad is attached (`play.ps1` sets it to `'0'`).
  * **`work\gt7ctl.ps1` updated:** `tap` now holds for `-TapMs` (default 150 ms). Mouse remains the only unsolved channel (see cont.10).
  * **Consequence for the perf work too:** every scripted route (menus, DLC, driving) is now automatable, so future A/B sessions need no help from Mark.
* **2026-10-15 (session 7 cont.12) — three more route facts, then hand back to Mark.** (a) The emulator's "Sign in to PlayStation Network — unavailable"
  dialog appears **TWICE** in a row: `winkey Return`, brief wait, **`winkey Return` again**. Cross does nothing on the second one (the pad map never
  applies to the emulator's own dialog) — that was my error. (b) The Display Settings assistant needs **D-pad Down + Right held together, THEN Cross**,
  as two separate actions, on both the crosshair screen and the Black Point Adjustment screen. (c) **`KYTY_HOST_INPUT_ONLY=1` is REQUIRED** for the
  synthetic pad to be the pad the game reads; without it the game reads Mark's physical pad and scripted presses look like they do nothing.
  (d) Pressing **Esc** in the emulator window closes the emulator; and **aborting a foreground tool call kills the emulator that call launched** —
  launch from a background task, or just relaunch after an abort. Tooling: `work\gt7watch.py` (change-driven driver, saves a BMP per step),
  `work\gt7ctl.ps1` (presses), `work\gt7route.txt` (route). **Route automation is not yet reliable end-to-end; Mark still drives the boot for now.**
* **2026-10-15 (session 7 cont.13) — `QUIET_LOCKSTEP` A/B in a real 3D scene: NO fps gain. It is a dead end for speed.** Run `gt7you` (Mark driving,
  the agent toggling `KYTY_DRAW_RUN_QUIET_LOCKSTEP` live every 12 s, 16 samples). Scene: "Welcome to Gran Turismo 7" — the camera sweep over the
  scenery/track (a dense 3D scene, ~7 fps).
  * **ON: n=9, wall 144.6 +-28.3 ms, 7.4 fps. OFF: n=7, wall 150.2 +-43.0 ms, 7.3 fps.** The 5.6 ms difference is far inside the noise
    (sd 28-43 ms). Per-sample fps - ON: 8.4 9.0 5.7 5.2 8.4 7.0 7.5 9.1 6.3; OFF: 10.1 4.6 7.9 5.4 7.7 8.2 7.0.
  * An earlier hand-paired read of adjacent windows suggested +8-10 %; that was an **artifact**. Lesson (again): **only grouped, state-tracked
    analysis is valid** — pairing neighbouring windows invents wins. (The first grouped attempt was also wrong because the pre-3D menu windows
    leaked into the "off" group; filter to the A/B period by the `Live: seq ...` lines.)
  * **Verdict:** the change is mechanically correct (continuations 2 % -> ~20 %), **safety-verified** (0 mismatches over ~330k compared draws), but it
    **does not move the frame** in either the light 3D background or this dense 3D scene. Keep it only as a correctness-neutral improvement if upstream
    ever wants it; it is **not** a speed fix.
* **2026-10-15 (session 7 cont.14) — "skip straight to the 3D scene"? No shortcut exists.** (a) The emulator has **no savestate support**
  (grep for savestate/snapshot in `src` finds nothing), so there is nothing to snapshot/restore. (b) **GT7 does not persist progress**: it rewrites
  `LIVERYIV\sce_sys\param.bin` and `DRFILEIV\sce_sys\param.bin` on mount at every launch (16:33:27 today) but **`sce_sdmemory\1000\memory.dat` is never
  written** (still 08:35:03) — that 32 MB `sce_sdmemory` is its real settings store. So there is no save to restore us to the 3D scene.
  The only routes are: a fast reliable scripted boot (input channel now works; the step order is in §3), or getting GT7 to run its own save once and then
  keeping a backup of `_SaveData\PPSA01317` to restore before each launch (the `bench.ps1` trick used for Astro Bot).
* **2026-10-15 (session 7 cont.15) — real 3D-scene baseline captured while Mark played (run `gt7save`), plus the save watcher.** Mark drove the 3D
  Music Rally scene at ~6 fps:
  * `CpGaps`: `wall 172.6-227.7 ms/frame` (5.5-5.8 fps), `commit 58.0-106.0 ms (3,549 draws/frame)`, `gap 111.0-121.7 ms =
    headwait.barrier 28.4 (2,880-2,934/f) + fault 20.0-28.3 (273/f) + gpuwait.drain 10.5-14.5 + protect 8.7 (2,492-2,572/f) + service ...`.
  * `Kyty fault cost: 6.1 fps: 818 faults/frame at 230.8 us (188.8 ms/frame)` and `5.1 fps: 1,316 faults/frame at 178.5 us (235.0 ms/frame)`;
    `protect 3,300-4,909 calls/frame`, `unprotect 2,124-2,784 calls/frame`.
  * `DrawRun: 169k-181k draws/10 s, 43.6k-48.2k eligible, 31.3k-34.8k key matches, continued 0.4 % / 0.3 %` (defaults; quiet-lockstep off).
  * **This is the most fault-dominated scene measured so far**: the CP's own `fault + protect` is ~29-37 ms/frame (vs ~9 ms in the earlier 3D scene), and
    the guest side is 189-235 ms/frame summed over its threads. → **the guest write-fault path is the leading suspect for this scene (lead 1c)**; the
    CP-side waits (`headwait.barrier` 28 ms, `gpuwait.drain` 10-14 ms) are known not to limit.
  * **Save capture:** watcher `logs\savedata_watch.txt` (started 16:44:13) shows **NO write to `sce_sdmemory\1000\memory.dat`** — only the boot-time
    `DRFILEIV/LIVERYIV\sce_sys\param.bin` rewrites at 16:44:43. GT7 had not been exited via its own menu at the time of writing, so the save-capture
    question is still open.
* **2026-10-15 (session 7 cont.16) — save data: GT7 does NOT persist, and the capture route is blocked. Guest-log probe run started.** Over Mark's
  full session (Music Rally driven, "You can now play Gran Turismo 7" progress dialog, back out to the World Map) the save tree watch
  (`logs\savedata_watch.txt`) showed **no write to `sce_sdmemory\1000\memory.dat`** — only the boot-time `param.bin` rewrites, and those are written by
  the **emulator's own** `SaveDataSetParam`, not by the guest, so they prove nothing. At boot GT7 *reads* `/savedata0/DRFILEIV.dat` (which does not
  exist) and therefore concludes "no save" and re-runs the first-run flow. So the "capture GT7's save and restore it" skip is **blocked** until its
  write is understood.
  * **Probe run `saveprobe2`** launched WITH the guest log so the save sequence + return codes can be read:
    `.\gt7fm.ps1 -Tag saveprobe2 -NoFaultMap -NoProfile -CpStats -EmuArgs '--printf-direction','File','--printf-output-file','C:\Users\mark\KytyPS5\logs\saveprobe2_guest.txt'`
    (guest log `logs\saveprobe2_guest.txt`, grew to 65 MB in ~15 s; expect several hundred MB).
  * **Tooling bug found and fixed:** `work\gt7fm.ps1` had **no `-EmuArgs` parameter**, so a `-EmuArgs` passed to it was silently ignored (the emulator
    command line showed only `--game/--screen-width/--screen-height`). It now has `[string[]]$EmuArgs` appended to `$emuList`. **Check the running
    process's command line (`Get-CimInstance Win32_Process`) whenever an emulator flag "does nothing".**
  * **What GT7's save actually does (guest log `saveprobe2_guest.txt`, guest-time 00:01:54, for `DRFILEIV` and then `LIVERYIV`):**
    ```
    SaveDataMount3(user_id=1000, dir_name=DRFILEIV, blocks=673, mount_mode=33)  -> /savedata0
    SaveDataPrepare(prepare_mode=0)
    SaveDataGetParam(param_buf_size=1328, title="Gran Turismo® 7", user_param=203)
    SaveDataSaveIcon(buf, buf_size=384825, data_size=384825)     <- 384 KB icon blob
    SaveDataCommit(commit_mode=0, mode=0)
    SaveDataUmount2(/savedata0)
    ```
    ...and later it **opens `/savedata0/DRFILEIV.dat`** (the progress file), which does not exist. So GT7 *creates* the save container (param + icon +
    commit) but never writes a progress/data file, and its 32 MB `sce_sdmemory` is only ever read. **Leading explanation: GT7 flushes its memory save on
    process EXIT (`sceSystemServiceExit`/application exit), and every run so far ended with the emulator being killed or crashing — so the write never
    happens.** Next experiment: let GT7 exit *itself* (its own quit option), with the save watcher running, and see whether `memory.dat` appears.
    (Side note: `SaveDataSaveIcon` is a stub in the emulator, so the created save has no icon — cosmetic, not the cause.)
* **2026-10-15 (session 7 cont.17) — AGENT DROVE GT7 FROM COLD BOOT TO THE 3D SCENE, unaided. Verified route + the two lessons.** With
  `KYTY_HOST_INPUT_ONLY=1` (so the game can only read the synthetic pad) I stepped a cold boot screen-by-screen, **looking at a screenshot before every
  press** (screens saved as `logs\look_A..Q.png`):
  1. PSN "sign-in is unavailable" dialog (**emulator's own** — pad does nothing) -> **`winkey Return`**
  2. black + GT logo + spinner -> **WAIT** (no press)
  3. PSN dialog **again** -> **`winkey Return`**
  4. GT7 "Failed / You cannot use network functions" (OK) -> **Cross**
  5. abstract intro animation -> **WAIT**
  6. **Display Settings** (crosshair, "facing straight at the television", Start) -> **Down + Right together, then Cross**
  7. **White Point Adjustment** (slider + Back/Next) -> **Down**, then Cross   *(Cross alone OPENS the slider — the focus starts on the slider)*
  8. slider opened by mistake -> **Circle** backs out
  9. **Black Point Adjustment** (same layout) -> **Down**, then Cross
  10. graphics priority -> **Cross** (Frame Rate is preselected)
  11. Your Settings (Resolution 4K / HDR Off / Frame Rate) -> **Right**, then Cross (Close)
  12. Music Rally "Please enjoy the Music Rally minigame" -> **Cross**
  13. Music Rally track list (track 1 Hooked on Classics / **Alsace**, rest padlocked) -> **Cross**
  14. **INITIALIZING…** (animated keyboard) -> **WAIT ~75 s** -> 3D scene ("Welcome to Gran Turismo 7. We will now set up your basic controls", Alsace).
  * **LESSON 1 (the big one): a route that presses blindly goes WRONG, because presses are consumed by WAIT pages** (the intro video, the copyright
    page). Mark spotted it: "you were clicking when the game was in the wait pages... so you are one in front". Fix: only act when the picture has been
    **stable** for a moment — a video never settles, a dialog does. Implemented as `gt7watch.py --settle-ms 800` (`wait_stable()`); the driver now sits
    through wait pages automatically.
  * **LESSON 2: on the Display Settings slider screens the focus starts ON THE SLIDER**, so Cross opens it instead of pressing Next — press **Down**
    first (the button then renders visibly brighter).
  * **To reproduce:** `.\gt7go.ps1 -Tag X` (launches + starts the driver detached), then read
    `logs\route_time.txt`. Keep a screenshot-per-step when changing the route.

## 14. Session 8 - why the driver pressed blind (3 bugs, all in the CAPTURE layer)

The driver was driving a **constant black square**: `BitBlt` from `GetDC(hwnd)` returns **all zeros**
for this Vulkan-rendered `SDL_app` window (nothing is drawn into the GDI window DC). `max byte == 0`
on every grab. Consequences, all of which looked like "route problems":

1. **Every screen digest was identical** -> `wait_change()` timed out on *every* step (the suspicious
   `changed after 6.0s` / `25.0s` values = the timeout, not a change) and the driver pressed blind.
2. **Every screen looked "settled"** -> the settle gate never did its job.
3. The window rect (not the client rect) was captured, so the title bar's live `frame:/fps:` counter
   would have made the picture look permanently changing anyway.

**Fixes (all in `work\gt7watch.py`):**
* Capture from the **screen DC** (`GetDC(0)`) + `BitBlt` at the window's **client** origin
  (`GetClientRect` + `ClientToScreen`), exactly as `capture.ps1` does. Verified: `max byte 255`,
  digest changes on real changes.
* `wait_stable()` now uses a **tolerance** (`diff_fraction(prev,cur) <= 1%`), not exact digest
  equality: emulator screens pulse slightly, so byte-identical never happens.
* "Screen is up" sync is **content-based** (`max(raw) > 40` or >0.3% of sampled pixels lit), not
  change-based: the first screen (the PSN dialog) is **static**, so waiting for movement hung forever.
  Note the PSN dialog is mostly black - only ~**1.4%** of pixels are lit; a 2% bar failed.

**The PSN dialog wants CROSS, not Return.** Verified on the live dialog: `winkey Escape` (PostMessage
WM_KEYDOWN to the SDL window) did **nothing**; a real `SendInput` Enter did **nothing**; `tap J` (the
file channel / pad map) **dismissed it and dropped straight into the 3D scene**. So the old route's
`RETURN` for screens 1-2 was simply the wrong button - and PostMessage to an SDL window is ignored in
general. **Use the file channel for everything**; it needs no focus.

Corrected start of the route (now in `work\gt7route.txt`): `J, J, J, DOWN+RIGHT, J, DOWN, J, ...`
* Also added to `gt7watch.py`: `SETTLE [timeout]` route step (wait for a settled screen, e.g. to sit
  out the shader compile) and bare `DOWN`/`UP`/`LEFT`/`RIGHT` steps (they were `?? unknown step`
  before, so the two Down-off-the-slider steps silently never happened).
* New tooling: `work\gt7go.ps1` (launch + start the driver **detached**, returns immediately - the
  driver must not be a child of a shell we block on, or killing that shell kills the emulator),
  `work\wintree.py` (enumerate a pid's windows: proved the emulator has ONE `SDL_app` window with no
  children, i.e. its dialogs are drawn in-window), `work\sendinput_key.py` (real SendInput keystrokes),
  `work\dialogprobe.py` (throw candidate keys at a dialog and report which one changes the screen),
  `work\mouseclick.py` (real SendInput click, prints where the cursor actually landed and which
  window/pid is under it).

### 14a. WHICH input channel works, per dialog (measured, not assumed)

| screen | owner | dismisses it |
|---|---|---|
| "Sign in to PlayStation Network / Cancel" | the emulator's own UI | **only a REAL mouse click** |
| "Failed / You cannot use network functions ... OK" | GT7 | pad **Cross** (file channel `key J`) |
| Display Settings / Music Rally / menus | GT7 | pad, file channel |

On the emulator's PSN dialog, tested and **all ignored**: file-channel keys (`key Return/J/Space/Down/
Right`), `PostMessage` WM_KEYDOWN, real `SendInput` keystrokes, and real `SendInput` mouse clicks (the
button *does* light up on hover, so the cursor is in the right place - the click itself is dropped).
Mark's physical mouse click dismisses it instantly. So full hands-off boot has exactly one manual
step: click that dialog, twice. `work\gt7route.txt` encodes it as `WAITDARK 300` x2 - the driver
blocks until the screen goes black (dialog dismissed) and then drives everything else.

**`frame:` in the window title is the reliable "the emulator is really rendering" signal.** Right
after launch the window exists but nothing is drawn, so a capture shows the **desktop behind it** -
bright - which fooled a content test into passing at t=0 and pressing keys at a blank screen.
`wait_ready()` now requires `frame_count(hwnd) >= 240` AND content AND settled.

**Coordinate trap:** screenshots are returned to me *displayed* at 1200x703 but are really 1296x759 -
multiply by **1.08** before converting to screen coordinates. Two clicks missed the button because of
this; `mouseclick.py` now prints the actual cursor position and the window/pid under it for checking.

**"Wait for movement" is wrong on this game.** The first screen (the PSN dialog) is *static*, so a
change-based sync waits forever; and two consecutive PSN dialogs are *identical*, so a change test
cannot tell them apart either. Sync on readiness (frame counter + content), then act on settle.

### 14b. CORRECTIONS to 14a - two conclusions above were WRONG

1. **The PSN dialog is NOT mouse-only.** It IS dismissed by a real **J** keystroke. The real reason
   nothing worked: `SetForegroundWindow` **fails silently** when another window owns the foreground,
   so the keystroke never reached the emulator (and my one real-key test had sent **Enter**, the wrong
   key). `focus()` now attaches to the foreground thread's input queue (`AttachThreadInput`), calls
   `SetForegroundWindow`, **verifies** `GetForegroundWindow() == hwnd`, and returns whether it took.
   With that, a real `SendInput` J dismissed the dialog immediately (screen changed 89%). Mark was
   right all along: "it did work when I clicked J before".
   * Also note: what made Mark's *mouse* click work was simply that a physical click carries the
     click itself - synthetic SendInput clicks on this emulator only ever produced hover.
2. **The first-run setup is NOT persisted.** GT7 runs Display Settings -> White Point -> Black Point
   -> graphics priority -> Music Rally on **every** boot. A run that went "PSN cancels + Cross ->
   3D scene" only did so because the setup had already happened *in that same session*; a genuine
   cold boot repeats the whole flow. Do not shorten the route - the full 13-step route is required.

### 14c. What works (verified end-to-end, cold boot -> 3D Alsace)

| step | channel | why |
|---|---|---|
| emulator PSN dialog x2 | **real J** (`focus()` verified + `SendInput`) | pad map and PostMessage never reach the emulator's own UI |
| GT7 "Failed ... OK" | pad **Cross**, file channel | GT7's own UI |
| Display Settings, sliders, priority, Music Rally | pad, file channel | same |

Route steps that implement "look, then press": `PSN` (wait for the dialog signature - a box on black,
**0.005 < lit < 0.06** and max>40; the bare boot page is also black, so the lower bound is essential),
`WAITPSN`, `WAITCHANGE`, `WAITDARK`, `GT7OK` (bright + static = GT7's dialog -> pad Cross; bright +
moving = the 3D scene -> stop), `SETTLE`, `SHOT`, `REALJ`.
**Measured:** the 13-step route from the Display Settings screen to the 3D scene took **47.5 s**
unattended (`logs\route2.txt`), ending on "Welcome to Gran Turismo 7" in Alsace at **9 fps** - the
performance problem from section 10, still open.

---

## 15. HANDOFF (end of 2026-10-15) - NOT SOLVED YET. Read this first.

**Two goals, in Mark's priority order:** (1) GT7 playable at the right speed - the 3D scene runs at
**9 fps** and that is the real prize; (2) an unattended cold boot to the 3D scene (currently *close*
but **not reliable**).

### 15.1 What is SOLVED and verified (do not redo this work)

* **Screen capture.** `BitBlt` from `GetDC(hwnd)` returns **all zeros** for this Vulkan `SDL_app`
  window. Capture the **screen DC** (`GetDC(0)`) at the window's **client** origin
  (`GetClientRect` + `ClientToScreen`). Without this, every screen looks like the same black square
  and every "why did it press that" investigation is wasted. `gt7watch.Grabber`.
* **Screen comparison.** Exact digests never match (screens pulse). Use `diff_fraction(prev,cur) <= 1%`
  for "settled". `wait_stable` does this.
* **Readiness.** Do not start acting at t=0: the SDL window exists before anything is drawn and the
  capture shows the **desktop behind it** (bright). Gate on `frame_count(hwnd) >= 240` (the `frame:`
  number in the window title) + content + settled -> `wait_ready`.
* **The input-channel matrix (measured, not guessed):**

  | screen | owner | what dismisses it |
  |---|---|---|
  | "Sign in to PlayStation Network / Cancel" | the emulator's own UI | **a REAL J keystroke** (`SendInput`), **not** the pad map and **not** `PostMessage` |
  | "Failed / You cannot use network functions" | GT7 | pad **Cross** via the file channel |
  | Display Settings / sliders / Music Rally | GT7 | pad, file channel |

  The pad file channel is `_kyty_input.txt` in the emulator's CWD (**`C:\Users\mark\KytyPS5-jetsku`**);
  the emulator drains it, so size 0 is normal. Syntax: `key <SDL key name> down|up`, `tap X`, `shake`.
  **Hold >= 150 ms** or a 60 Hz game never samples it.
* **Focus is a real trap.** `SetForegroundWindow` **fails silently** when another window owns the
  foreground, so real keystrokes never arrive. `gt7watch.focus()` attaches to the foreground thread's
  input queue (`AttachThreadInput`), sets the foreground, then **verifies** `GetForegroundWindow()`.
  With that, a real J dismissed the PSN dialog (89% screen change). Mark was right all along that J
  worked: "it did work when I clicked J before".
* **Tooling:** `gt7go.ps1` (launch + start the driver **detached**; never block on it or killing the
  shell kills the emulator), `gt7ctl.ps1` (`-Steps 'tap J','shot x.bmp'`), `capture.ps1`,
  `focus.ps1 -Proc kyty_gt7`, `wintree.py`, `sendinput_key.py`, `mouseclick.py`, `dialogprobe.py`.
  Route step vocabulary in `gt7watch.py`: `PSN`, `REALJ`, `WAITPSN`, `WAITCHANGE`, `WAITDARK`,
  `WAITSTATIC <ms>`, `WAIT3D`, `GT7OK`, `SETTLE`, `WAIT n`, `SHOT <path>`, `DOWN`/`UP`/`LEFT`/`RIGHT`,
  `J`/`I`/`K`/`L`, `X+Y`.

### 15.2 What is BROKEN (the actual work for next time)

**The state machine misidentifies screens and presses into pages that advance on their own.**
Mark, twice, verbatim: *"you were clicking J when it was on the animation bit and again when it was
on the copyright 3-5 seconds page that automatically goes"* and *"nope you did it again!! you click
when the animation and the copyright is on!!"*

Evidence (`logs\route_time.txt`, run fast15): `WAITPSN` "matched" at **4.1 s** and the `GT7OK` Cross
fired at ~**15 s** - both long before the game is at those screens. So the gates are matching the
wrong screens, and every press after that is one page out. Root cause: `WAITPSN`'s test
(`max > 40 and 0.005 < lit < 0.06`) and `GT7OK`'s ("bright and static for 6 s") are both satisfied by
early pages. **The copyright page is bright AND static for 3-5 s; the initialising screen is bright
and animating.** "Bright + static" cannot identify a page that waits for input, and adding a timeout
only guesses.

**Next step: MEASURE, don't guess.** `work\bootrecord.py` is written for exactly this and has been
**fixed but never successfully run** (its docstring previously had a bad `\N` escape - keep it `r"""`).
It logs `(t, lit, moving, max, frame)` every 0.5 s for 130 s, saves a capture every 5 s to
`logs\rec\`, and dismisses the PSN dialog itself with a real J:

```powershell
Get-Process kyty_gt7 -ErrorAction SilentlyContinue | Stop-Process -Force
.\gt7fm.ps1 -Tag rec1 -NoFaultMap -NoProfile -Extra 'KYTY_HOST_INPUT_ONLY=1'
Start-Process 'C:\Users\mark\AppData\Local\Programs\Python\Python311\python.exe' `
  -ArgumentList '-u','-I','-X','utf8','C:\Users\mark\KytyPS5\work\bootrecord.py','130' `
  -RedirectStandardOutput 'C:\Users\mark\KytyPS5\logs\bootrec_run.txt' -NoNewWindow
```

Then read `logs\bootrec.txt` + `logs\rec\*.bmp` and build a **per-screen fingerprint** for each of:
PSN dialog, initialising animation, copyright page, GT7 "Failed" dialog, Display Settings crosshair,
White Point, Black Point, graphics priority, Your Settings, Music Rally prompt, track list, compile
screen, 3D scene. Each press should be gated on **its own screen's fingerprint** (lit fraction /
brightness layout / static-for-longer-than-any-auto-page), never on a bare timeout. Only then is
"look, then press" actually implemented.

### 15.3 Also open

* **The 9 fps 3D scene** - the original goal, untouched today. From section 10: the scene is
  fault-dominated, ~1,000-1,400 guest write faults per frame at ~84 us each, each one a GPU readback
  through `BufferCache::InvalidateMemory` -> `ReadMemory(vaddr, size, true)` (`bufferCache.cpp`
  1436-1444). That is real work, not a wait, and it is where the speed is.
* **`vgamepad`/ViGEmBus** - the driver was installed on 2026-10-15 (Nefarius Virtual Gamepad
  Emulation Bus + an XINPUT HID device) but is **not needed**: everything above works through the
  file channel + real keystrokes. It could not be disabled without admin rights. It may or may not
  affect pad enumeration - worth remembering if the file channel ever seems dead.
* **Route steps that were needed only while the game had not stored its setup** - none: the first-run
  setup (Display Settings -> sliders -> priority -> Music Rally) runs on **every** boot.

## 16. Session 9 (Claude Sonnet 5.5, 2026-10-16) - boot timeline MEASURED + SendInput bug
* `bootrecord.py` now runs (its docstring contained a literal `r"""` that closed the string early - fixed). Run: `bootrec1` in `logs\bootrec.txt`, frames `logs\rec\*.bmp`.
* **Cold-boot timeline (measured):** ~0-85 s the capture is **pure black (lit 0.0000)** while the frame counter climbs (shader/pipeline warm-up); the emulator's
  **PSN dialog first appears at ~88 s** (lit ~0.015, then static). So the old "WAITPSN matched at 4.1 s" was a false match on black. Gate: wait for lit 0.005-0.06 AND static,
  never before ~60 s.
* **BUG FOUND: `sendinput_key.py` `INPUT` struct was 32 bytes; x64 needs 40** -> `SendInput` returned 0 / GetLastError 87, i.e. every "real J" from this module was silently
  rejected. Fixed (union padding 24 -> 32). `mouseclick.py:26` has the same `c_byte * 24` - NOT yet fixed.
* After the fix `SendInput` returns 1 and focus verifies True, but the PSN dialog was **still not dismissed** in my (Claude Code, non-interactive shell) test; the file channel `key J` also does nothing for this dialog (as documented).
  A small bright strip on the left edge of captures suggests another window overlaps the emulator - check the capture isn't picking up an overlay. Unresolved.

### 16.1 RESULT (Claude Sonnet 5.5, 2026-10-16): UNATTENDED COLD BOOT -> 3D SCENE WORKS (69 s, verified)
* **New driver `work\gt7auto.py`** replaces the timing/brightness state machine. It grabs the screen, compares a 160x90 grayscale thumbnail to a saved **reference image per screen**
  (`work\screens\*.png`) and presses only when one reference has matched 3 samples running, with a per-screen threshold (half the distance to the nearest other reference, clamped 3..15)
  and a margin over the runner-up. A screen with no reference (black load, GT logo, intro video) matches nothing -> it waits, so it can no longer press on auto-advancing pages.
  The copyright page is itself a reference with NO action. "Done" = bright, unmatched, after `initializing` was seen *following* the track pick. Log: `logs\gt7auto.txt`.
* **Input channels (measured):** emulator PSN "sign-in unavailable" dialog = **real mouse click on Cancel** (`mouseclick.py`; client x=50%, y=56.25%); its keys (J/Return) did NOT work from this shell.
  GT7's own dialogs/menus = **pad file channel** (`gt7watch.press`, J = Cross). Order seen: PSN dialog -> logo -> PSN dialog again -> GT7 "Failed" (J) -> copyright (wait) -> Display Settings.
* **Bugs fixed:** `sendinput_key.py`/`mouseclick.py` INPUT struct was 32 B (needs 40 on x64 -> SendInput err 87); `bootrecord.py` docstring; `bootrecord.py` now clicks Cancel.
* **The "88 s black start" was disk spin-up** (game is on external D:/E: drive after a fresh PC start, per Mark). With the drive awake the PSN dialog is at ~4 s and the whole boot to 3D takes ~69 s.
* **Run it (no admin needed):**
  ```powershell
  cd C:\Users\mark\KytyPS5\work
  Get-Process kyty_gt7 -ErrorAction SilentlyContinue | Stop-Process -Force
  Start-Process powershell -WindowStyle Minimized -ArgumentList '-NoProfile','-File','C:\Users\mark\KytyPS5\work\gt7fm.ps1','-Tag','X','-NoFaultMap','-NoProfile','-Extra','KYTY_HOST_INPUT_ONLY=1'
  Start-Sleep 5
  Start-Process 'C:\Users\mark\AppData\Local\Programs\Python\Python311\python.exe' -ArgumentList '-u','-I','-X','utf8','C:\Users\mark\KytyPS5\work\gt7auto.py','400' -RedirectStandardOutput 'C:\Users\mark\KytyPS5\logs\gt7auto_run.txt' -WindowStyle Hidden
  ```
  Output `3D scene reached` in `logs\gt7auto_run.txt`. Helper `gt7step.py <name> [keys]` saves a reference of the current screen then presses keys (used to build `work\screens`).
* **Still open:** (a) 9 fps 3D scene (§15.3) is the real goal - now reachable in ~70 s. (b) Settings are re-asked every boot: GT7 only writes its save on exit (§13 save-data finding) - test a clean in-game quit.
  (c) The driver stops at the first 3D frame ("Welcome... basic controls"); it does not play the tutorial.

### 16.2 FPS benchmark harness + first A/B (Claude Sonnet 5.5, 2026-10-16)
* **`work\gt7bench.ps1 -Tag X [-Extra 'K=V',...] [-Settle 45] [-Seconds 60]`** = cold boot -> `gt7auto.py` -> settle -> sample the window-title `fps:` every 2 s -> `logs\X_bench.txt` (median/mean/min/max). Fully unattended (~3 min/run), so startup knobs can be A/B'd without Mark.
* Tutorial 3D scene, warm caches: baseline **median 6-7 fps** (mean 6.3-6.5; range 0-13, early compile stalls). `KYTY_DRAW_PREP_STEAL=1`: median 7 / mean 6.3 -> **no gain, dead end** (b0b vs b1steal).
* Run-to-run noise is about +-1 fps, so only gains > ~2 fps are real. The remaining levers are source changes (see §13 session 7: `instance` draws 81 ms/frame, per-draw lockstep `headwait.barrier`, GPU readback drains), not startup knobs.

### 16.3 Why the 3D scene is slow: the lockstep-read lead, MEASURED (Claude Sonnet 5.5, 2026-10-16)
**Bench harness fix:** `gt7bench.ps1` originally passed `-Extra 'A=1,B=1'` through `-File`, which delivers ONE string, so extra env settings were **silently not applied**; the first `STEAL=1` and `UNSAFE` results (b1steal, b6unsafe) are VOID. Fixed by passing the array through `-Command`. Verify an extra took effect (e.g. the counter line it removes) before trusting an A/B.
**Instrumentation added (marked `GT7-INVESTIGATION`, in `graphicsRun.cpp`, `gpuTouchedPages.h`, `coherenceLog.h`; copies in `work\gt7-investigation-session9\`; the exe `kyty_gt7.exe` is the instrumented build, the previous exe is `kyty_gt7.pre-session9.exe`):**
 * `GT7BARRIERS` (every 10 s): ~13-20k self-label waits, ~4-5k other waits, ~65-100 flip waits, and **~420-670k "cond/other" ops** per 10 s (~7,000/frame at ~7 fps).
 * `GT7LOCKSTEP`: the "cond/other" ops are almost all **`LockstepRead`** = the command-processor front reading guest memory the GPU may have touched. Two sources:
   shader **register-pair reads** (`ReadRegisterPairs` -> `ReadGuestForFront`): **~400-560k per 10 s, avg ~120 B, spread over ~900-1000 different 64 KiB regions** (no single hot spot), and **command-buffer copies** (`CheckCommandBytes`): ~55-90k per 10 s, ~600 B each.
 * Why they are lockstep: `GpuTouched::g_pages` is a **sticky, never-cleared 4 KiB-page bitmap**; any page ever marked by a GPU transition forces in-order (resolver) reads. **100 % of GT7's reads hit pages marked by BOTH `BufferDirtyAdd` and `PublicationBegin`; `TrackerGpuMark` and `ImageGpuModified` = 0.**
   `BufferDirtyAdd` (`bufferCache.cpp:3743`) marks the WHOLE range of every writable buffer binding, even when the shader writes a part (the precise `written` sub-range path at `:3783` exists but is not what GT7 mostly hits).
**Upper bound (valid A/B, 60 s, warm caches):** `KYTY_GT7_UNSAFE_DIRECT_READS=1` (skips ALL lockstep reads, UNSAFE - may read stale bytes) **mean 8.6 / median 9 fps vs baseline mean 7.3 / median 8** (b7unsafe vs b7base). So even removing every one of these reads is worth only **~+1-2 fps (+18 %)**.
**Conclusion: do NOT build a "precise touched" fix for lockstep reads - best case +1-2 fps, with real correctness/concurrency risk.** The frame is dominated by the **per-draw commit (3,600 draws/frame x ~40 us)** plus GPU readback drains. Getting GT7 to a playable rate needs the upstream-style *reuse prepared draw state across frames* idea (`upstream-prepared-draw-cache.md` is a ready draft; GT7 never continues a run, `cont` 11/f) - a large change.
* **Corrected `STEAL` A/B (valid, b8steal vs b7base):** `KYTY_DRAW_PREP_STEAL=1` mean 7.2 / median 8 vs baseline mean 7.3 / median 8 -> **no effect (confirmed dead end).**

### 16.4 GPU-side picture of the 3D scene (Claude Sonnet 5.5, 2026-10-16) - resolution is NOT the problem
* `nvidia-smi` during the scene: **GPU 46-100 % (mostly ~62 %), 185-195 W of a 200 W limit, SM clocks 1590-1920 MHz vs 2130 max (power-limited)**. The older note "GPU nearly idle" was from compile stalls only.
* `KYTY_GPU_OP_PROFILE=3` (`work\gpuops_summary.py <dir>` summarises `KytyPS5-jetsku\_Profiling\gpuops-*\*.csv`), 8 captures: **~104 ms/frame of op deltas = ~55 ms `cb_begin` gaps (GPU WAITING for the CPU between command buffers; 187 cmdbufs/frame) + ~48 ms real GPU work**:
  compute `dispatch` **24.4 ms (617/frame, ~40 us each)**, draws ~12 ms (**all the 3840x2160 draws together = only 9.6 ms for 2,206 draws**), `barrier2` **8.1 ms (12,800/frame)**, buffer copies ~3.5 ms.
* **GT7 renders at 3840x2160 regardless of what the emulator reports** (`KYTY_VIDEOOUT_RESOLUTION=1`, added to `VideoOutGetOutputStatus`, changed nothing; `GT7RTSIZES` line shows ~55-70 % of draws at 3840x2160, plus 2048x2048, 512x512 and some 4352/8704 shadow-like targets). Because 4K draws cost only ~10 ms/frame, **a render-resolution scale would NOT help much** - do not build one.
* **So: the frame (~140 ms) is CPU/command-processor bound; the GPU needs only ~48 ms/frame of real work -> ceiling ~20 fps even with an infinitely fast CPU path.** Beyond that the GPU's own compute (24 ms) and barrier count (12.8k) become the limit.
* Current exe `kyty_gt7.exe` is an INSTRUMENTED build (GT7BARRIERS/GT7LOCKSTEP/GT7RTSIZES lines + `KYTY_GT7_UNSAFE_DIRECT_READS` + `KYTY_VIDEOOUT_RESOLUTION`); clean fallback = `kyty_gt7.pre-session9.exe`.

### 16.5 Submit-batching knobs A/B (valid, 60 s, instrumented exe) - all dead ends
baseline (d4base) mean 6.3 / median 6 | `KYTY_SUBMIT_MIN_INTERVAL_US=2000` mean 6.6 | `KYTY_IDLE_FLUSH_DRAWS=64` mean 7.2 | `KYTY_EOP_FLUSH_BATCH=64` mean 6.1. Baseline alone ranges 5.8-7.3 across runs, so none of these is distinguishable from noise (+-1 fps). No device losses. Do not pursue submit batching via env knobs.
Where the profile says the time really is (see 16.3/16.4): CP thread command-commit + waiting for the DrawPrep worker per draw (~3,000 lockstep hand-offs/frame), 270-390 render-pass starts/frame ("instance" draws ~210 us each), 15 GPU-readback drains/frame (~24 ms), 187 cmdbufs/frame.

### 16.6 The readback ping-pong, MEASURED - and why the frame stays ~7 fps (Claude Sonnet 5.5, 2026-10-16)
* `GT7DRAIN` (instrumentation in `BufferCache::ReadMemoryDrain`) + `KYTY_FAULT_MAP=1`: **~600-760 CPU-reads-of-GPU-data drains per 10 s waiting ~1.0-1.9 s in total (10-18 % of wall time)**, almost all from two small GPU-written buffers:
  `0xec06000000` (16 KB, **22 guest faults/frame, ~1.1 ms each, guest thread Job#0, guest rip 0x900507353**) and `0xf43a2e0000` (5 faults/frame, ~2.2 ms each, Job#0, rip 0x90030a34f); plus write-fault drains on `0x120aff0000` (~150/10 s). (Addresses move between runs - the 64 KiB block base differs per launch - the *pattern* is stable.)
  Reading: GT7 runs a **GPU-driven loop ~27 times per frame**: a compute pass writes a tiny result, the CPU reads it back, builds the next step from it. Inherent dependency; on a PS5 the round trip is microseconds.
* **It is real GPU backlog, not wake-up latency:** average **3.6-4.2 queued ticks** (command buffers) at each drain. `KYTY_GT7_DRAIN_SPIN_US=3000` (poll the timeline instead of blocking, in `MasterSemaphore::Wait`, for drains only) -> mean 7.1 vs baselines 6.9 / 7.4: **no effect**.
* Frame model consistent with all the data: CPU/CP path (~90 ms) and GPU work (~48 ms) mostly serialized by these ~27 round trips -> ~140 ms. A real fix means decoupling the readback from the queue backlog (copy on a separate/async queue after waiting only for the *writer's* tick, not `CurrentTick`) - a new multi-queue mechanism, not a tweak. Not attempted. Prior A/B (`KYTY_READBACK_FLUSH_SIDE=1`) already showed that shortening the drain just moves the wait elsewhere.
* **State of the tree:** `kyty_gt7.exe` was restored to the pre-session-9 build (`kyty_gt7.pre-session9.exe` is the same file). The instrumented build (GT7BARRIERS / GT7LOCKSTEP / GT7RTSIZES / GT7DRAIN, `KYTY_GT7_UNSAFE_DIRECT_READS`, `KYTY_VIDEOOUT_RESOLUTION`, `KYTY_GT7_DRAIN_SPIN_US`) is `kyty_gt7.instrumented-session9.exe`; its source edits (all marked `GT7-INVESTIGATION`) are still in `KytyPS5-src` (`graphicsRun.cpp`, `gpuTouchedPages.h`, `coherenceLog.h`, `renderDraw.cpp`, `bufferCache.cpp`, `masterSemaphore.cpp`, `videoOut.cpp`) - the NEXT build of `gt7-build.bat` would include them. Remove them or keep them knowingly.
* **Everything tried this session that did NOT move fps (all valid A/Bs, +-1 fps noise):** DRAW_PREP_STEAL, submit interval / idle-flush / EOP batch knobs, reported output resolution, unsafe skip of all lockstep reads (+1-2 fps best case), drain spin-wait.

### 16.7 `KYTY_READBACK_FLUSH_SIDE=1` re-tested with the valid harness (Claude Sonnet 5.5, 2026-10-16)
The "async readback copy that waits only for the writer" idea ALREADY EXISTS (`TryIssueSideReadback`, `bufferCache.cpp` ~276-300, 1700-1800; `KYTY_READBACK_FLUSH_SIDE`, default off). Valid 60 s A/B: **FLUSH_SIDE=1 mean 7.8 and 6.9 (two runs) vs baseline mean 7.1** (g1side/g2side/g3base) -> **no real gain**, although it removes almost all CP drains (**~550-560 drains / 2.5-3.1 s waited per 10 s -> ~75-90 drains / ~0.06 s**). **One Device loss in 3 runs (g1side)** - another reason to leave it off.
So the wait just moves from the command processor to the guest thread (Job#0), which still has to wait for the writer to run on the GPU, and the frame is unchanged: the round trips are serialized behind the GPU's real work and the CP's own throughput (~118 ms/frame CP thread), not behind a removable stall.
Also noted: `nvidia-smi` utilisation ~62 % of a ~140 ms frame = **~85 ms of GPU busy time per frame**, more than the op-profile's ~48 ms of op deltas, so the GPU side alone limits this scene to roughly 12 fps before any CPU improvement; the 200 W power limit (clocks 1590-1920 vs 2130 MHz) is a real, if modest, part of that.

### 16.8 Sampling profiler + CPU-layout A/B (Claude Sonnet 5.5, 2026-10-16)
* **`work\cpsample.py`** (external, NO emulator rebuild): `list` (thread CPU %), `sample tid:<n> <s> <out>` (SuspendThread + GetThreadContext @ ~1 kHz), `probe <exe> <s>` (sample every thread >8 % CPU), `report <samples> <exe> [n]` (llvm-symbolizer, inlining aware, LLVM output style = name/location line pairs). Needs the PDB of the running exe (`KytyPS5-src\_build_191\kyty_emulator.pdb` belongs to the LAST build = the instrumented one). `gt7sample.ps1` orchestrates boot + sampling. OS thread names are NOT set (GetThreadDescription empty) - identify threads by what they run.
* **Caveat:** lld ICF folds identical functions, so spin-wait loops show up under unrelated names (e.g. `LodStatsCounter::CounterBuffer` = a worker's idle spin, `WaitSlow`).
* **Hot threads in the 3D scene (one 4 s probe each):** CP/front thread 71 % CPU, **32 % of it in `DrawPrep::Engine::ReadyHead`** (waiting for the worker that claimed the head draw), then `ParseParts` 2.9 %, `Ring::WaitConsumed` 2.5 %, `FindImagesInRegion` 2.1 %, `CommitBindings` 1.8 %, `ObtainBuffer` 1.3 %; 29 % of its samples are outside the exe (driver). Sequencer thread 64 % CPU, **55 % in `Sequencer::WaitSlow`** (waiting for the resolver's answer); two DrawPrep workers ~62 % CPU each, mostly idle-spinning; 87 %-CPU thread is ~100 % inside the driver/kernel.
* **CPU-layout A/B (5950X, preset pins pipeline to CCD1 and guest to CCD0):** all on CCD1 mean 5.8, all on CCD0 6.1, unpinned 5.9, preset 5.7 (same batch) -> **no effect (cross-chiplet latency is not the limiter).** NB this batch ran lower overall (baseline 5.7 vs ~7 earlier), because Mark was using the PC / the first run was disturbed - compare only within a batch.
* `gt7auto.py` fixes this session: refocus before every press + verify the screen changed + retry (4x); **re-read the window origin on every look** (moving the window made the driver capture another window and never match).
* `kyty_gt7.exe` restored to the clean pre-session-9 build again; the instrumented exe is `kyty_gt7.instrumented-session9.exe`.

### 16.9 Stacked fixes, the GPU becomes the limit, and the hot shader (Claude Sonnet 5.5, 2026-10-16)
* **Bottlenecks are chained; removing one exposes the next.** With `KYTY_GT7_UNSAFE_DIRECT_READS=1` (no ordered front reads; UNSAFE, may read stale bytes; one device-loss crash in 2 runs) no emulator thread is above ~44 % CPU any more (probe `s4`): mean 8.1-8.6 / median 9-10 fps (max 12-14) vs ~6.5-7.1. Adding `KYTY_READBACK_FLUSH_SIDE=1` on top does NOT add (7.9-8.4) but pushes the GPU queue backlog from ~4 to 17-20 ticks.
* **GPU in the scene (`gt7gpu.ps1`, nvidia-smi aligned to the scene):** baseline util 59 %, avg 1738 MHz, 193 W; **stacked (unsafe+side) util 81 %, avg 1612 MHz (dips to 855-1200), 194 W of the 200 W limit** -> in that configuration the GPU is the bottleneck and is POWER-LIMITED. A higher power limit (Mark's decision; needs admin `nvidia-smi -pl`; card has a documented hardware fault, see AGENTSGPU.md) would now help; untested.
* **Hot shader:** `cs:0x618f2427a8040fcf` = **14.5 ms/frame (18.7 launches/frame, ~780 us each, 35 % of all draw+dispatch GPU time)**, launched back-to-back in a chain with barriers between (groups 323,44,17,402,39,16,10,2,...), each launch 700-1,200 us **regardless of group count**. SPIR-V (423 KB, 21,048 lines disassembled to `work\shaderdump\cs618f.spvasm`, dumped with `gt7dump.ps1` = `--graphics-debug-dump true` + `KYTY_PROGRAM_CACHE_VERIFY=1`, files in `KytyPS5-jetsku\_Shaders\`, **~3,800 files - delete when done**): LocalSize 32x1x1, ONE function, 14 loops, only 4 stores / 830 loads, **562 `OpGroupNonUniformBallot`, 414 `OpArrayLength` (per-access bounds checks), 4,996 `OpBitcast`, 2,000 `OpPhi`, 1,546 `OpSelect`**. A long serial per-thread program; why it takes ~0.8 ms is NOT yet explained (candidates: dependent load chains, ballot-emulated wave64 semantics, bounds-check overhead). Tools: `cpsample.py`, `shader_cost.py`, `one_shader.py`, `ctx_shader.py`, `barrier_summary.py`, `symrva.py`.
* **Barriers:** ~14,600 `barrier2`/frame, ~11,700 of them single-image layout transitions (`Image::Transition` -> `BatchImageBarriers`, `image.cpp:437`, `context.cpp` ~370; the batcher flushes when the same image is already pending or the call is non-deferrable). Total GPU cost only ~8 ms/frame.
* `kyty_gt7.exe` restored to the clean pre-session-9 build; instrumented = `kyty_gt7.instrumented-session9.exe`.

### 16.10 GPU power limit test (Mark's decision, 2026-10-16) - NOT worth it now
Mark raised the cap 200 -> 240 W (card repadded 3 weeks earlier, stable since; memory temp watched in GPU-Z; `nvidia-smi` cannot read memory temperature on this card - it prints N/A). Clean exe, two 60 s runs (`w1base240`, `w2base240`, `gt7gpu.ps1`): **mean 6.7 / 7.2 fps = no gain** (CPU-bound baseline), clocks steady ~1,915 MHz (vs 1,740 with sag at 200 W), avg 212 W, core max 70 C. **Both runs ended in an ErrorDeviceLost a few seconds after the measurement window** (`WriteInvalid at 0x0`, `vkQueue*`/`upload DMA semaphore`); at 200 W this appears only occasionally (~1 run in 8-10 in this session, also with instrumented/unsafe builds). Cause undetermined (emulator bug vs power/clock stability). **Reverted to 200 W.** Revisit only if the GPU becomes the real limiter with a SAFE stack of fixes. `gt7gpu.ps1` aborts the game at core temp >= 85 C and reports power/clock/util; it cannot guard memory temperature.

### 16.11 Stability fix, movie skip, controller, drive keep-alive (Claude Sonnet 5.5, 2026-10-16)
* **GPU crash ("3D WIDTH ZT Violation", ErrorDeviceLost) ROOT CAUSE FOUND AND FIXED (in `renderDraw.cpp`, `ClampRenderAreaToRealExtent`, called from `AcquireRenderTargets` for every colour and depth attachment).** The render area came from the GUEST image description, but the cache image can be smaller. Logged example: `RenderAreaClamp: depth slot 0 render area 197x75 exceeds the real attachment extent 186x75 (image 186x75 ...): clamped`. The Windows System log for the earlier crash (nvlddmkm id 13, "3D WIDTH ZT Violation" on GPCs 0,1,2,4,5 simultaneously) matches. With the clamp: 0 device losses in the following ~25 min of play. This is the same signature §9 of this file recorded as "emulator bug in render-state setup". The clamp only ever shrinks the area to the real attachment size and logs each occurrence (first 40).
* **Controller:** `gt7boot.ps1` no longer sets `KYTY_HOST_INPUT_ONLY=1` (that variable makes the game read ONLY the synthetic pad; benchmark scripts still set it on purpose). With 0 the real controller works and the file-channel automation still works.
* **Movie skip (`avPlayer.cpp`, `SkipLongMovieStart`):** `KYTY_SKIP_LONG_MOVIES_MS=<n>` starts any movie with a video track and duration >= n ms from its last `KYTY_SKIP_LONG_MOVIES_TAIL_MS` (default 3000) ms. GT7's first-run intro is a 388,371 ms (6.5 min) un-skippable movie; `gt7boot.ps1` sets 240000. Log line: `AvPlayer: skipping a long movie (388371 ms): starting at 385371 ms`. Verified: World Map loads straight after.
* **Save data:** GT7's `memory.dat` (32 MB) has NEVER been written (all zeros); the containers DRFILEIV/LIVERYIV are re-created on every boot because they are empty. The emulator's save code writes immediately when called (`SaveDataSetSaveDataMemory2` -> `save_memory`), so GT7 simply never calls it during the boot/setup/music-rally/movie path. Reached: setup -> Music Rally "You can now play" -> title (OFFLINE MODE: World Map / Music Rally) -> intro movie -> World Map. Still no data write at that point. Next test: first menu book / first car at the Cafe, `work\savewatch.ps1` logs `logs\savewatch.txt`.
* **Slow black start = external drive waking up (D:).** `work\keepdrive.ps1` writes 16 bytes to D:\.keepalive every 30 s so the drive never idles.
* **Driver (`gt7auto.py`) now re-reads the window origin on every look** (moving the window made it capture another window); it re-focuses before each press and retries.
* The current `kyty_gt7.exe` = build with clamp + movie skip + the old GT7-INVESTIGATION instrumentation (extra log lines, small overhead). Clean fallback: `kyty_gt7.pre-session9.exe` (has the crash!).

### 16.12 Dropping the hot shader's chain, and why optimising it alone is a small win (Claude Sonnet 5.5, 2026-10-16)
* `KYTY_GT7_SKIP_CS=<hex hash>` (diagnostic, `renderCompute.cpp`, drops every direct dispatch of that compute shader; picture wrong on purpose). A/B 50 s, clean-ish build with the ZT clamp: baseline **mean 7.0 / median 7**, GPU util 62 %; with `618f2427a8040fcf` dropped **mean 8.3 / median 9 (+19 %)**, GPU util 53 %. So the entire chain (GPU time + its barriers + the readbacks attached to it) is worth ~20 % of the frame; even an infinitely fast version of that shader cannot give more.
* Shader facts (`work\shaderdump\cs618f.spvasm`): all 414 buffer accesses use CONSTANT indices into `buffers[]` (buffer 2: 332 accesses, 1: 56, 0: 22, 3: 4), so the runtime-descriptor-array is not the cost; no `NonWritable`/`Restrict` decorations (loads cannot be reordered against stores); one `OpArrayLength` bounds check per access; 14 loops; 232 `flattened_srt` reads. A plausible cause of ~0.8 ms/launch is long dependent global-load chains in the loops (RDNA scalar-cache loads become ordinary global loads) - unproven.
* **Correction to 16.10:** the two device losses at 240 W were very probably the ZT-width bug (fixed since by `ClampRenderAreaToRealExtent`), not the power limit. Worth re-testing 240 W with the clamp build if the GPU becomes the limiter.
* **Tally of measured ceilings (each alone, same harness):** order-free front reads ~+18 %, hot-shader chain ~+19 %, side-copy readbacks ~0 alone (but frees the CP once the others are done), GPU power limit ~+10 % estimated only in the stacked config. Stacked best case ~10-12 fps for this scene. A 30 fps scene needs a different order of change.

### 16.13 CHECKED SPECULATIVE FRONT READS - the first safe frame-rate gain (Claude Sonnet 5.5, 2026-10-16)
* **Measurement first (`GT7SPEC` line, `ReadGuestForFront`):** ordered (lockstep) reads were compared with a speculative direct read of the same bytes: **~400-530 k reads per 10 s over a 90 s run, 0 differ.** (GT7's register-pair data sits on pages marked "GPU-touched" only because writable bindings mark their WHOLE range - `bufferCache.cpp:3743` - not because the GPU writes them.)
* **Implementation (opt-in, `KYTY_SPEC_FRONT_READS=1`, `graphicsRun.cpp` + `cpOps.h`):** `ReadGuestForFront` (after `AwaitPendingWrites`) reads the bytes DIRECTLY and continues parsing; it emits a non-lockstep `ReadCheckOp{address,size,hash,spec=1}`; the resolver executes it IN ORDER (`case OpKind::ReadCheck`), re-reads the authoritative bytes with `ReadGuestForCp`, compares XXH3 hashes. **On a mismatch it logs `SpecFrontReads: MISMATCH ...` and switches speculation OFF for the rest of the run** (the draws built from that one read may be wrong; no rollback). Progress line every ~524 k checks: `SpecFrontReads: N speculative reads checked in order, M mismatches`. Not used in verify mode (`KYTY_CP_SEQ_VERIFY`) or for reads > 4096 B. Command-buffer copies (`CheckCommandBytes`) are unchanged.
* **Result (60 s runs, same harness, no device losses in any of these runs):** baseline mean 6.6 (earlier baselines 6.3-7.3); **speculation on: 9.2, 7.1, 8.0 -> avg ~8.1 (about +20 %)**; 6.3 M checked reads, **0 mismatches**; with `KYTY_READBACK_FLUSH_SIDE=1` as well: 7.1, 9.0 (no extra gain).
* `gt7boot.ps1` now enables it together with the movie skip and the dialog auto-dismiss. Remaining measured ceilings: hot shader chain `618f2427...` <= ~+19 %, GPU power limit ~+10 % (stacked, untested with the ZT fix).

### 16.14 GPU power limit retest WITH the ZT clamp (Mark set 240 W; memory temp watched in GPU-Z) (Claude Sonnet 5.5, 2026-10-16)
* With `KYTY_SPEC_FRONT_READS=1`: at 200 W the GPU was pinned (util 73-77 %, avg 1,675-1,728 MHz, 191-192 W of 200 W): mean fps 8.6 and 8.1. At **240 W: mean 9.1 and 9.3 (median 10 both), clock avg 1,807-1,877 MHz, power avg 212-216 W (never reached the limit), core max 71 C, memory temp 82-88 C (GPU-Z, Mark), NO device loss in either run** (the two crashes at 240 W earlier were the ZT render-area bug, not power). Gain ~ +10 %.
* The load only draws ~212-216 W, so a limit above ~240 W buys nothing for GT7. `nvidia-smi -pl 240` is manual and the boot task re-applies the 200 W cap at every reboot; memory temp for AI loads is a different story (AGENTSGPU.md: 280 W AI = 106-108 C) - set 200 W again for AI.

### 16.15 `flattened_srt` NonWritable (recompiler) and the post-recompile warm-up rule (Claude Sonnet 5.5, 2026-10-16)
* Change: `spirvEmitterModule.cpp` marks the flattened SRT storage buffer `NonWritable` (it is only ever loaded). Safe; the program cache invalidates itself (key = hash of recompiler sources) and ~7,700 shaders were recompiled.
* **Result: no measurable gain.** After the caches settled (12-minute convergence run `conv1`, per-minute fps from the `Kyty fault cost: last N s, X fps` lines in `logs\<tag>.out` - a free fps time series): 6.9, 7.2, 6.4, 4.4, 8.2, 7.4, **10.0, 9.2, 8.8, 9.8, 9.6** => last 5 min avg ~9.35 vs ~9.2 before (240 W, speculation on). Kept (harmless).
* **Rule: after ANY recompiler change the game needs 10+ minutes of play before benchmarks are valid** (program cache re-fills, the driver recompiles every pipeline at first use; `gt7warm.ps1` once is NOT enough - two benchmark attempts at +5 and +30 min still read 2-5 fps with 0-fps stalls). Do not benchmark until the minute-by-minute fps is stable.
* **New observation (spec reads ON): the DrawRun fast path now works for GT7: `continued 26-27 %` of draws (was 0.3-0.4 %; the ordered reads counted as foreign activity and broke every run).** Funnel in a 10 s window: 402,848 draws -> 148,066 eligible (37 %) -> 117,093 key matches -> 107,677 continued. Remaining misses: activity=8,598 (LockstepRead 6,662 = the command-byte copies `CheckCommandBytes`, WaitRegMem 963, service 931). Next levers: (a) make `CheckCommandBytes` copies speculative+checked too, (b) raise eligibility (63 % of draws are not "eligible to seed a run"), (c) KYTY_DRAW_RUN_RECALL (off).

### 16.16 The draw-run funnel, evening of 2026-10-16 (Claude Sonnet 5.5) - how GT7's 63 % ineligible draws were traced
Diagnostics added (all `GT7-INVESTIGATION` in `renderDraw.cpp`, safe to remove): **`GT7ELIGIBLE`** (first rule each draw fails for draw-run eligibility), **`GT7LATE`** (why a started continuation falls back late: command/tick, images, indirect barrier; plus WHICH image field differs).
Funnel with `KYTY_SPEC_FRONT_READS=1` (per 10 s, ~400k draws): 26 % continue (3 us/draw vs 9-70 us). Of the rest, **about 90 % fail ONE rule: `prog:fault-buffer`** (the shader has the emulator's fault-buffer binding; 233k and 172k draws per window); address-writes, image writes, GDS, mesh, indirect, clears etc. fail ~0.
* Change 1 (opt-in `KYTY_DRAW_RUN_FAULTBUFFER=1`, `DrawRunProgramEligible`): a program whose only blocker is the fault buffer may run when `KYTY_DRAW_RUN_WRITES` is on (it then counts as a "writing" program). Verify mode (`KYTY_DRAW_RUN=verify`): **342,850 checks, 0 mismatches.** Continued draws 26 % -> 53-55 %. fps unchanged (9.0 / 8.2 vs 8.1 / 8.6) because late fallbacks went from ~40 to ~100,000 per 10 s.
* `GT7LATE`: of ~190-260k continuation checks per 10 s, `images changed` ~100-157k (command/tick ~2.5k, indirect barrier ~2.4k). **100 % of those are `texture-refresh` on a TEXTURE (never an attachment)**: `DrawRunTextureReady` (`renderDraw.cpp` ~2321) refuses any image with `depth_id` or `HasStencil()` (depth/stencil textures = GT7's shadow maps) although `TextureBindingMemo::RefreshIsNoOp` (the authoritative mirror of `RefreshImage`) has no such clause and CPU-dirty/buffer-modified are checked separately (fields 12/13).
* Change 2 (opt-in `KYTY_DRAW_RUN_DEPTHTEX=1`): accept depth/stencil textures in `DrawRunTextureReady`. Result: see 16.17 (pending when this was written).
* Tools added today: `winshot.py` (capture any window; `--screen` copies from the screen DC, PrintWindow returns black for GPU-Z), `gpuwatch.ps1` (watchdog on GPU-Z's live sensor log `Documents\GPU-Z Sensor Log.txt`: columns 3 GPU temp, 4 hot spot, **5 MEMORY temp**, 15 board power; >= 94 C mem / 83 C core -> `schtasks /run /tn GPU-PL-200` + email; >= 100 / 90 C also closes the emulator), `notify.ps1` (email Mark via SQL Database Mail), `gt7warm.ps1`, `gt7gpu.ps1`.
* Remote access: Mark enabled Claude Code **/remote-control**; the session can be reached from the Claude app/browser at the session URL (Mark does not receive push notifications, so long jobs end with an EMAIL).

### 16.17 Wider draw-run fast path: RESULT (Claude Sonnet 5.5, 2026-10-16 evening)
`KYTY_DRAW_RUN_FAULTBUFFER=1` + `KYTY_DRAW_RUN_DEPTHTEX=1` (+ `KYTY_SPEC_FRONT_READS=1`, GPU limit 200 W):
* **Verify mode (`KYTY_DRAW_RUN=verify`): ~430-490 k checks per 10 s window, 0 mismatches, no `DrawRunVerify` message.** Late fallbacks 100,000 -> ~2,000 per 10 s; `images changed` 0.
* Funnel per 10 s (~427 k draws): eligible 400 k (94 %), key matches 252 k, **continued 236 k (55 %)** (was 26 %).
* fps: with both 9.3 / 9.4 (medians 10 / 11) vs same-batch baseline 8.9 (earlier 200 W baselines 8.1-8.6). **About +5-10 %.** Per-draw commit is no longer the limiter; the scene is limited by GPU work and the GPU->CPU round trips.
* `gt7boot.ps1` now enables all three switches. Remaining ceilings: hot shader chain (+<=19 %), the 27 GPU->CPU round trips per frame, GPU power (200 -> 240 W ~ +10 %, Mark's call).

### 16.18 Hot shader `cs:0x618f2427a8040fcf` - what the SPIR-V says (offline analysis, Claude Sonnet 5.5, 2026-10-16 night; no game needed)
* Loop trip counts: `%2919 = OpSelect (srt[39] < 32) srt[39] 32` i.e. **`min(value from the flattened SRT, 32)`**; every loop is `for (i = 0; i < min(n, 32); i++)` per virtual half-wave (the shader emulates wave64 as two 32-bit halves inside one 32-wide subgroup: every variable has a pair of phis). So each launch is up to 14 loops x <= 32 iterations of ~300-600 SPIR-V instructions each, **per thread**, whatever the group count -> launch cost independent of size (measured 0.7-1.2 ms for 2..402 groups).
* Per iteration: ~40 `OpGroupNonUniformBallot` (exec-mask / lane-predicate emulation; 562 static), ~30 loads (buffers[2] 332 static accesses + 232 `flattened_srt` loads), one `OpArrayLength` bounds check per buffer access, many `OpSelect`/`OpPhi` (predication).
* Rough budget per thread: up to 18 k dynamic ballots (~20 cycles each in a dependent chain) + a few k dependent global loads -> hundreds of microseconds. Consistent with the measurement; not provable without a GPU profiler (Nsight Graphics, free, gives per-instruction stall reasons - NOT installed).
* Ideas, ranked by plausibility (none implemented): (1) **CSE identical ballots** in the SPIR-V emitter when the predicate is the same value in the same block; (2) **hoist loop-invariant `flattened_srt` loads** (done by the compiler now that the buffer is NonWritable - measured 0 gain, so they are not the cost); (3) **specialise per dispatch**: if at bind time the written buffer's range does not overlap the read-only ones, compile a variant with separate NonWritable+Restrict read-only buffers (needs the emitter to split the `buffers[]` array and the descriptor layout; high effort; `Aliased` currently prevents the host compiler from assuming anything); (4) ask whether the lane loop can run at the guest's real wave size instead of 2x32 emulation (only valid if the shader is wave-size independent).
* Verdict: **option B is a low-probability / medium-effort item** unless Nsight is available to show where the stalls are. Ceiling is still +19 % for the whole chain.

### 16.19 Power limit retest with EVERYTHING on (valid run, 2026-10-16 20:08) - Claude Sonnet 5.5
`power_ab.ps1` (detached; restores 200 W in a `finally`; emails Mark): all three switches (`KYTY_SPEC_FRONT_READS`, `KYTY_DRAW_RUN_FAULTBUFFER`, `KYTY_DRAW_RUN_DEPTHTEX`), 90 s samples:
**200 W: mean 9.1 / median 10 | 240 W: mean 9.6 and 10.0 / median 12 and 11 (max 15).** GPU-Z memory temp max 86 C (watchdog), no watchdog action, no device loss. Gain ~ +5-10 %. Limit restored to 200 W. Earlier failures of this very test were NOT the emulator: the PC's display turned off after 15 min of no input / the session locked, so screen capture saw black or the Windows lock screen (the "lanterns" picture is Windows Spotlight, NOT the game).

### 16.20 Settings sweep with all three switches on (detached `sweep.ps1`, 200 W, 2 interleaved reps each; 2026-10-16 night)
base (SPEC_FRONT_READS + DRAW_RUN_FAULTBUFFER + DRAW_RUN_DEPTHTEX) **9.1** (9.8 / 8.4) | `KYTY_IDLE_FLUSH_DRAWS=2` 8.35 | `KYTY_IDLE_FLUSH_DRAWS=0` 8.2 | `KYTY_READBACK_FLUSH_SIDE=1` 8.9 | `KYTY_EOP_FLUSH_BATCH=2` 8.2. No device losses. **None of these helps; do not retry.** Note the base itself ranged 8.4-9.8 between two runs, so single-run differences < ~1 fps are noise; use >= 2 interleaved reps.
New launcher option: `gt7boot.ps1 -SkipHotShader` (drops the launches of `cs:0x618f2427a8040fcf`, ~+19 % measured earlier; picture may be wrong; needs a human look).

### 16.21 Barrier flood diagnosis and range-merge experiment (2026-10-17)
- `FlushBarriers` return-address histogram (`GT7FLUSH`, in context.cpp, investigation code): ~1.1 M non-empty flushes / 10 s with exactly 1 image each; caller = `CommandBuffer::BatchImageBarriers` (same-image flush at the loop head, RVA ~0x43a27). My earlier `GT7BARRIERIMG` "same image pending" counter only looked before the loop, so it under-counted: one batch carries many subresource ranges (mips/layers) of the SAME image and each used to force a flush.
- Fix tried: flush only when aspect+mip+layer ranges overlap (`KYTY_BARRIER_RANGE_MERGE=1`). Flushes dropped ~8x (1.1 M -> 0.13-0.16 M per 10 s, 57-68 images per big barrier), 0 device losses.
- RESULT: SLOWER. Interleaved 2 reps at 200 W: off 8.4 / 9.0 fps, on 6.9 / 7.0 fps. Likely the driver/GPU handles one huge multi-image barrier worse than many small ones (or it serialises more). Left in the code as OPT-IN (default off). Do not enable. Lesson: fewer barrier calls != faster; barrier2 10.7 ms/frame is probably driver cost per image, not per call.
- Rebuilt: kyty_gt7.exe now has the switch default OFF.

### 16.22 Lead 3 (GPU->CPU round trips): the hand-off latency is NOT the problem (2026-10-17)
- New probe `src/common/gt7Timeline.h` (`GT7TL` line every 10 s; hooks: `bufferCache.cpp` ReadMemoryDrain after the wait + `ReadMemory` scope end, `graphicsRun.cpp` SendCommandSync, `renderContext.cpp` HandleFault read branch, `queueSubmission.cpp` before vkQueueSubmit). It follows one read fault: GPU finished -> command done -> guest thread woken -> ReadMemory ends -> fault handler returns -> next vkQueueSubmit.
- Clean result (thread-local filtered, 500-600 round trips/10 s = ~8.5 per frame): drain-end->cmd-done ~18-36 us, ->guest wakes ~8 us, ->ReadMemory end ~40 us, ->handler returns ~25 us, ->next submit ~70-80 us. TOTAL ~0.17 ms per round trip = ~1.5 ms of a ~110 ms frame (about 1.3 %).
- So thread wake-ups, semaphores, fault handling and the CP hand-off are cheap; no spin-wait / priority trick can give more than ~1-2 %. (Earlier unfiltered readings of 700-1000 us were an artefact of other threads' ReadMemory calls advancing my state machine.)
- Conclusion: the ~19-33 ms/frame the CP spends in drains is the GPU genuinely executing the serial dependent chain (hot shader 21 x ~765 us + its neighbours); the guest cannot continue until it finishes. The only lever on that chain is GPU time per step (lead 2: the hot shader), or overlapping the chain with independent work that GT7 does not offer.
- Side findings: barrier range merge also tested (16.21), no gain.

## 2026-10-24 night (Claude, solo): GT7 on int21 + gt7-spike port - NOT yet better than kyty_gt7.exe
- Baseline (same route, same intro scene, warm cache): previous build `kyty_gt7.exe` (jetsku) = **7.28 fps**, GPU 190 W busy, text renders (`gt7_old.png`).
- Stock int21 + our Oct-14 patches (`kyty_int21p.exe` before the port): GT7 intro scene **~1 fps, GPU idle (31 W)**, no text (font fix missing) - `gt7_d`.
- Ported gt7-spike onto `int21-base` (UNCOMMITTED working tree, `git cherry-pick -n` / `git apply --3way`; patch snapshot to be saved as `work\int21-gt7port-uncommitted.patch`):
  `91b051ff` (profiler init order), `e6e1f12a` (boot spike + font metrics; libFont.cpp conflict merged with upstream `resolve_stb_glyph`: main face uses
  `stb_metrics_scale`), `ada5ab05` (kerning), `62e56b11` (speed work: drawRun/renderDraw/bufferCache/masterSemaphore..., conflicts in bufferCache.cpp (keep both),
  renderDraw.cpp (took upstream; spike's GT7LATE diagnostics dropped), videoOut.cpp (resolution override + upstream dynamicRange), colorRenderTarget.cpp (EQAA:
  accept num_samples >= num_fragments, keeps upstream's 2x approximation log)); pipelineCache.cpp hand-applied (MaxDriverCacheFileSize 2 GiB, KYTY_PROGRAM_CACHE_MAX_MB).
  Skipped: diagnostics-only commits (probes, KYTY_BALLOT_CSE, NULL_AUDIO/SKIP_DRAWS, thread names). Compiles clean.
- Result `gt7_e` (merged build): text renders, scene OK, but **2.25 fps avg** (0-9 fps bursts), GPU 43 W idle -> still stalled. Next: bisect the 12 new int21 preset keys
  (`gt7_f` = all 12 off), then `kyty_int21p` thread stacks while stalled (cdb -pv), compare CP behaviour with gt7-spike.
- Scripts: `work\gt7_int21.ps1 -Label x [-Extra ...]` (launch+PSN Return pulses+route+settle+measure on int21p), `gt7_old.ps1` (baseline on kyty_gt7), `gt7_continue.ps1`.
  `gt7route.ps1` got `-BootWaitS` and `-DelayScale` (cold cache: the PSN sign-in dialog appears minutes after launch); `gt7ctl.ps1`/`key.ps1` honour `$env:KYTY_INPUT_DIR`.
- ErrorDeviceLost on a cold-cache int21p GT7 run (`gt7_c`, tick 589282) = the known GT7 issue (§9), clone fired once (3 blocks) - not proven unrelated; re-test with `KYTY_CFG_CLONE=0` if it recurs.

### CORRECTION (2026-10-25 00:10): the CFG clone does NOT hurt GT7; earlier "stall" was cache warm-up
- Earlier in this session I concluded that `KYTY_CFG_CLONE` stalled GT7 (runs `gt7_e` 2.25 fps / `gt7_f` 0.0 vs `gt7_g` 4.3 with the clone off). **That comparison was invalid:** the runs
  happened in sequence on a GT7 cache (`KytyPS5-int21\_PipelineCache\PPSA01317.*`, ~1.5 GB programs + 1.1 GB driver cache) that kept getting warmer, and the program-cache
  fingerprint did not include the clone switch then.
- Fair pair on the warm cache, same route/settle/measure (`gt7_int21.ps1`, settle 5 min, 120 s): `gt7_h` default (clone off) **7.36 fps**, GPU 194 W; `gt7_i`
  `KYTY_CFG_CLONE_REGIONS=1` **8.36 fps**, GPU 158 W. Previous build `kyty_gt7.exe` (`gt7_old`): 7.28 fps. => int21 + gt7-spike port = parity with the old build; clone = neutral/slightly better.
- RULES for GT7 measurements on a new exe/cache: (1) the first runs are cold-cache and read 0-2 fps with an idle GPU - settle until `Pipeline journal ... replay built 0` and a stable fps; (2) never A/B
  across runs while the cache is still growing; (3) the cfg_clone_regions option now IS part of `CodegenFingerprint` (`cfg_clone_regions1`), so on/off use separate cached programs.
- The clone stays OPT-IN (`KYTY_CFG_CLONE_REGIONS=1`, `CodegenOptions::cfg_clone_regions`) until it has been checked on more games; recommended ON for R-Type (2.4 -> 28 fps Hangar), neutral on GT7.
- Fair A/B of int21's 12 new preset keys (warm cache, clone on, settle 5 min, 120 s): `gt7_i` keys ON **8.36 fps** (GPU 158 W) vs `gt7_j` keys OFF **6.50 fps** (GPU 195 W) => the keys are worth ~+29 % on GT7; keep the int21 preset as shipped.
- Summary of this session's GT7 numbers (same route/scene): old `kyty_gt7.exe` 7.28 | int21p port, clone off 7.36 | clone on 8.36 | clone on + keys off 6.50. Target is still 50 fps; the limiter is the submit/complete latency loop (see playbook 10j), not the GPU.
