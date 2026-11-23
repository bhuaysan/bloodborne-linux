# Recording Vulkan commands off the GPU thread (#676)

At `--frame-rate 120` the clinic's `play-stand` segment misses 120 FPS because the GPU command thread (`shadPS4:GpuComm`) needs about 9.25 ms per frame, against a budget of 8.33 ms. #678 and #679 took the low-hanging cuts. What remains is spread across all 1716 draws of a frame at about 5.4 µs each: resource binding, the pipeline lookup, the driver's draw calls, vertex binding and render pass setup, none above a fifth of the thread. Further cuts of 2–3 % each were mostly within run-to-run noise. This note decides how to move the recording of Vulkan commands to a thread of its own.

## What can move

The GPU thread does two kinds of work per draw:

- **Resolving guest state into host objects.** PM4 parsing, `BindResources` (texture and buffer lookups, uploads of CPU-written pages, write tracking), the pipeline key and lookup, image layout tracking. This reads and changes the buffer cache, the texture cache and the page manager. It has to run in command order, and those caches belong to the GPU thread (#629). It stays.
- **Recording the result.** `vkCmd*` calls into radv: bind pipeline, push descriptors, push constants, vertex and index buffers, dynamic state, begin and end rendering, barriers, copies, draws and dispatches. This takes resolved handles and values and changes nothing the resolving side reads. It can move.

A perf run of `play-stand` and `play-walk` at 120 (local run during #679, before the color write masks were baked) puts 21 % of the GPU thread's samples in `libvulkan_radeon.so`, plus about 1 % in our own recording code (`Scheduler::BeginRendering`, `DynamicState::Commit`). #679 cut radv's draw cost, so about 19 % of 9.25 ms, roughly 1.75 ms, moves. The copies into the stream and staging buffers (`memmove`, about 12 % of the thread) are CPU writes on the resolving side and stay.

Moving recording adds cost on the GPU thread: every command's arguments are written into a buffer instead of being passed to radv. At about 8 commands and 0.6–1 KB of arguments per draw (descriptor writes and push data are the bulk), that is about 14 000 small records and 1–1.7 MB per frame, estimated at 0.3–0.4 ms. **Expected result: `play-stand` at about 7.8–8.0 ms**, under 8.33 ms with a margin of about 0.4 ms. The margin is thin. If step 2 misses the issue's check, the next cut is the `Matches` check below, before anything else moves. The cheaper `Matches` check on the pipeline lookup's repeat path (0.3–0.6 ms, #676 handoff) adds to that margin and is independent of this change.

## Decisions

**One recording thread, not several.** After the split the GPU thread is again the bound, at about 8 ms of resolving, and the recording thread runs at about a quarter of a core. More recording threads would only help if recording were the bound. They would also need secondary command buffers per render pass, which add driver cost, and an order across threads for barriers that the caches insert between draws. Parallel resolving would need thread-safe caches, which is a rewrite of the buffer and texture caches and is not planned.

**A deferred command buffer with the same interface, not a lambda per call site.** `Scheduler::CommandBuffer()` and `UploadCommandBuffer()` return a `DeferredCommandBuffer` instead of a `vk::CommandBuffer`. It has the methods of the subset the renderer uses (about 35: `bindPipeline`, `draw*`, `dispatch*`, `pushDescriptorSetKHR`, `pushConstants`, `bindVertexBuffers*`, `bindIndexBuffer*`, `setVertexInputEXT`, the dynamic state setters, `beginRendering`, `endRendering`, `pipelineBarrier*`, the copy, fill, update, clear and resolve commands). Each method writes an opcode and a copy of its arguments into the current chunk. Pointers and spans (descriptor writes with their buffer and image infos, push data, `updateBuffer` data, copy regions, barriers, rendering attachments) are copied by value. The about 80 recording sites in `vk_rasterizer.cpp`, `vk_runtime.cpp`, `vk_pipeline_common.cpp`, `blit_helper.cpp`, `tile_manager.cpp`, the host passes and the presenter's draw path keep their code, so the change stays reviewable. A wrapper per call site with captured lambdas would touch every site and give the compiler less to check. A record that cannot be copied by value fails to compile, so no pointer into GPU-thread state can reach the recording thread.

**Chunks are handed over while the frame is built.** The GPU thread flushes once per frame (`submit_done` in `Liverpool::Process`), so handing over only at a flush would start the GPU a whole replay later. Chunks of 64 KiB go to the recording thread as soon as they are full and at every `EndRendering`. The recording thread replays them into the current `VkCommandBuffer`. A flush appends a submit record that carries the `SubmitInfo` and the tick. The recording thread ends the command buffers, submits under `Scheduler::submit_mutex` (the queue is shared with the presenter's schedulers) and starts the next ones. The recording thread is idle when there is no chunk and does not spin.

**Ticks stay where they are.** The GPU thread still assigns the timeline value at its flush (`CurrentTick()`, `NextTick()`), and every lifetime rule keeps working on it:

- Destruction is already deferred by tick: images and their views (`TextureCache::DeleteImage`), the blit helper's views, buffer cache writes, staging and stream buffer reuse (`StreamBuffer::WaitPendingOperations`, `StagingBufferPool`) and command pool reuse all wait until the GPU has passed the tick of their last use. A tick the GPU has passed was submitted, and so was every record before it. So no handle in a chunk not yet replayed can be destroyed. This is the invariant the change rests on: **a Vulkan handle the GPU thread records may only be destroyed through `DeferOperation` or another tick check.** A run with `--validation` reports a violation as the use of a destroyed handle.
- `IsFree(tick)` asks the GPU's semaphore, which cannot be past a tick the recording thread has not submitted. It stays conservative.
- `Wait(tick)` makes sure the tick is flushed (as today) and waits on the timeline semaphore. Vulkan allows a host wait before the signal is submitted, and the recording thread never waits for the GPU thread, so this cannot deadlock.

**Command pools belong to the recording thread.** Vulkan requires external synchronization of a pool, and only the recording thread allocates, begins and ends command buffers. The upload command buffer had no callers left and went in step 2, so a submit carries one command buffer. A scheduler that is destroyed lets its recording thread replay and submit what is queued, then waits for the last tick before the pool goes.

**Only the draw scheduler defers.** The presenter owns three schedulers (`vk_presenter.cpp`): `draw_scheduler`, which the rasterizer and the frame's post-processing use on the GPU thread, and `present_scheduler` and `flip_scheduler`, which record a few commands a frame on the present thread. Only `draw_scheduler` gets a recording thread. The other two keep recording directly, so a `Scheduler` takes its mode at construction. Calls on the returned buffer that are not commands, such as `reset()` in `~Presenter`, become `Scheduler` methods that run after the recording thread has drained.

**The submit callback stays on the GPU thread, and every queue operation takes `submit_mutex`.** At each submit the rasterizer's callback (`SetSubmitCallback` in `vk_rasterizer.cpp`) records the runtime's pending barriers and calls `BufferCache::SubmitPendingArenaBinds`, which calls `bindSparse` on the graphics queue and adds the wait for its semaphore to the `SubmitInfo`. Both read and change GPU-thread state, so the callback runs on the GPU thread when it appends the submit record, not on the recording thread. Its barriers go into the chunk like any other command. `bindSparse` then reaches the queue before the commands of earlier flushes that the recording thread has not submitted yet. That is safe only while sparse binds only add pages that no queued command uses, which is how the arenas grow now; a bind that removes or moves pages would have to go through the recording thread as a record. Today `bindSparse` runs inside `SubmitExecution` and so under `submit_mutex`; after the split it has to take the mutex itself, because the recording thread submits to the same queue at the same time. The same holds for `swapchain.Present()`, which already takes it. `vkDeviceWaitIdle` needs every queue externally synchronized as well, so `Swapchain::Destroy` and `ImGui::Core::Shutdown` take the mutex around it, and `Present` recreates the swapchain only after it has let go of the mutex.

**The present thread waits until the frame's tick is submitted.** `PrepareFrame` sets `frame->ready_tick` and flushes `draw_scheduler`. The present thread's submits in `Present` and `ReleaseFrame` wait on that tick. Today the tick is on the queue by then. After the split the recording thread may not have submitted it yet, and a queue wait on a timeline value whose signal is not submitted makes radv hold the submit back in its own submission thread, which adds latency to every present it touches. So `draw_scheduler` keeps a submitted tick (an atomic the recording thread sets after each submit, with a condition variable), and the present thread waits on it before it submits. Host waits (`Wait(tick)`) don't need this.

**Where the recording thread runs.** #610 showed that load on the SMT sibling of the GPU thread's core slows the GPU thread down. The recording thread runs at about a quarter of a core, enough to cost part of the gain if the scheduler puts it there. **Decision (#695): it stays unpinned.** A probe build moved the recording thread's affinity off both threads of the core the GPU thread last ran on, whenever that core changed (about 60 changes a second), and counted how often the two threads shared a core. Unpinned, they shared one for 1–3 % of the recording thread's work items on a quiet machine. In clinic play at 120, three quiet runs per placement gave the same GPU-thread CPU per present within the spread between runs (`thread_cpu.py`, ms):

| placement | `play-stand` | `play-walk` |
|---|---|---|
| unpinned | 7.85, 8.02, 7.55 | 6.39, 6.52, 6.16 |
| kept off the GPU thread's core | 7.70, 8.33, 7.55 | 6.13, 6.50, 6.00 |

The spread between runs of one placement (0.5–0.8 ms) is larger than any difference between the two. The scheduler rarely puts the threads together because the GPU thread is busy most of the frame, so pinning buys nothing measurable and adds an affinity call each time the GPU thread migrates. Under outside load the unpinned threads shared a core more often (6.5 % of work items in a run with a video render beside it), but such runs are not comparable anyway (`docs/test-rig.md`, "Thread times").

**The synchronous paths get slower by the queue's length.** `Finish()` flushes and waits, so it now also waits for the recording thread to replay what is queued, up to one frame's records in the worst case. The callers are the GDS read at an end-of-pipe event (`liverpool.cpp`), a write fault on GPU-written memory (`BufferCache::FlushGpuModified`, #537), synchronous downloads and the texture cache's garbage collector. They are rare in the measured scenarios, and the rig measures them (below). If one of them shows, the recording thread can replay ahead of the hand-over (taking a partly filled chunk) on a `Finish`.

**Measured (#695): they don't show.** `yharnam-route` at 60, three quiet runs each with the recording thread and with `--sync-recording` (the replay and submit on the GPU thread, as before the thread) on the same build: the median p99 of every play segment is within 0.03 ms of the other side (16.69–16.74 ms; `play-walk` 18.69 against 18.72 ms, with single runs from 16.8 to 20.8 ms on both). The `fault` part of a normal frame is 4.0–5.1 ms on both sides. Replaying ahead on a `Finish` is not needed.

**A switch for comparison runs.** `bb-run --sync-recording` replays each chunk on the GPU thread at hand-over and submits there, as `--sync-pipelines` does for pipeline builds (#521). It rules the thread out when a run draws wrongly or hangs.

**30 and 60 FPS keep their behaviour.** The commands, their order and the submits are the same. Only the thread that calls radv changes. The GPU starts a frame's work slightly later, by the time it takes to replay the last chunk.

**Staging copies of CPU-written buffer pages run on copy threads (#777).** A buffer sync reserves staging space, protects the pages again, records the `vkCmdCopyBuffer` and hands the `memmove` from guest memory to one of two copy threads (`CopyQueue`, owned by the scheduler). Every submit carries the ticket of the last copy job pushed before it and goes to the queue only once every job up to that ticket is done: the GPU reads staging space only after the submit, so replaying the chunks before it need not wait. With the recording thread that wait is on the recording thread; with `--sync-recording` it is on the GPU thread at the submit. The GPU thread also waits for every job pushed so far wherever the command processor makes progress the guest can see (`Liverpool::WaitCopies`: EOP, EOS and ReleaseMem labels and their interrupts, WriteData, DumpConstRam, memory semaphores, the streamout size write, occlusion dumps, the flip NOP, DmaData fills and copies done on the CPU, a finished submit and GPU idle). Before, the inline `memmove` had read a draw's buffers before the command processor got past it; a game that sees a label may write the next frame's data into a buffer a draw before the label read, so its copy has to be done by then. These points come a few times a frame, mostly after its draws, so most of the copying still overlaps the GPU thread's work. The other waits are where the GPU thread waits for the GPU anyway (`Finish`, `Wait`). A CPU write during a job faults and marks the page modified again, so the next draw syncs it again (before, a write during the inline `memmove` was not caught either). Staging space is still reused by tick: a tick the GPU passed was submitted after its jobs were done. Only uploads of 64 KiB or more go to the threads: in `yharnam-route` at 60 they carry almost all of the `memmove` time (about 1000 a second, 350 KiB on average), and a smaller one takes about as long as the hand-over. Detiling stays on the GPU thread: on `yharnam-route` the `detile` part is 1–10 µs per frame on average and 136 µs at most, too little to move.

## Steps

1. **The deferred command buffer, replayed synchronously** (Core). `DeferredCommandBuffer`, its records and replay, `Scheduler` returning it, replay at hand-over on the GPU thread. Nothing moves to another thread yet. Unit tests replay recorded streams against lavapipe and compare the results with direct recording. Measured: the record cost on the GPU thread, as the change of `commands` against `main`; the `frame_profile` part `replay` times the Vulkan calls that step 2 moves. **Gate:** if it costs more than 0.5 ms per frame in `play-stand` at 120, stop and rethink before step 2.
2. **The recording thread** (Core). Chunk hand-over, submit records, the thread, pool ownership (`UploadCommandBuffer()` still returns a raw command buffer after step 1 and has no callers: convert or remove it), `--sync-recording`. Tests: `Finish`, `Wait` and deferred destruction across the thread boundary, a submit with no records, shutdown with queued chunks, a sparse arena bind while chunks are queued, and the present thread's wait for a tick the recording thread has not submitted yet.
3. **Measurement** (in step 2's PR). `rig.py perf clinic-fight --frame-rate 120`, three runs per side: the issue's check. 30 and 60 against `main` (`compare main-menu`, `perf clinic-fight` at 60). Fault paths: the `fault` part of `frame_profile` and the p99 of `yharnam-route` at 60. The present interval at 120 against step 1, so a late frame from the present thread's wait shows. The thread placement runs above.

Each step is one PR under the 800-line limit.

## Alternatives

- **More per-draw cuts.** Each is 2–3 % or less, and most tried in #678 and #679 were within noise. Not enough for 8.33 ms with room to spare. The `Matches` cut is still worth taking on its own.
- **Several recording threads with secondary command buffers.** See above: recording is not the bound after the split, and barriers between draws need one order.
- **Native render hooks** (engine render functions calling Vulkan directly, `AGENTS.md`). They remove the resolving cost for the passes they replace, but each pass is its own decompilation work. They stay the later step for passes whose measurement justifies them, not a way to the 120 FPS of the whole frame.

## Reusing pipeline lookups (#780)

At 120 FPS the heaviest segments of Central Yharnam still miss the 8.33 ms slot after the recording thread and the copy threads, with the GPU thread as the bound (#654, `docs/test-rig.md`, "Heavy areas at 30, 60 and 120"): `commands` is 9.2–10.2 ms of a normal frame, and `pipeline`, `vertex` and `state` are 31–33 % of it (1.48, 0.97 and 0.63 ms in `yharnam-route` `play-walk`). The plan was a scanner thread that runs ahead of the GPU thread with its own copy of the registers and computes the pipeline key, the flattened user data and the vertex input layout of coming draws, which the GPU thread takes when a fingerprint of the inputs still matches. This section decides against that thread and for a reuse table on the GPU thread.

### Where the time goes

Perf samples of the GPU thread (`yharnam-route` at 120, one #654 run, callers split by the innermost known function):

- **`pipeline`, the lookup: 17 % of the thread.** About 7.5 % of it reads guest memory and compares: `StageSpecialization::Matches` with the sharps behind the user data and the fetch shader's words (6.2 %), and the runtime info compare (1.3 %). The other 9.7 % works from registers only: building the key and the stages' runtime infos (`RefreshGraphicsKey`, `RefreshGraphicsStages`, `BuildRuntimeInfo`), the program table lookup by hash and `FindPermutation`'s own part. Finding the shader's binary info (0.7 %) and copying the flattened user data (0.4 %) are small.
- **`vertex`: 10 %**, nearly all of it the buffer cache (`ObtainBuffer`, syncs of CPU-written index buffers). The vertex input layout itself is under 1 %.
- **`state`: 1–2 %**, writing the descriptor and push records into the deferred command buffer.

### Why no scanner thread

A precomputed result is only usable once the GPU thread has checked that its inputs are still the same. For what the lookup reads from guest memory (the sharps, the fetch shader, the SRT tables of the flattened user data) the check reads the same memory as the computation and costs about as much: comparing a copy is as dear as making it. That part stays on the GPU thread whoever computes it. The registers are the part worth saving, and the GPU thread can tell for itself that they did not change: it writes them. A table of earlier lookups by register state gives the same saving with no second PM4 interpreter, no copy of the register file, no hand-over per draw and nothing that depends on how far ahead another thread got. The vertex input layout and the descriptor writes are too small to move, and the cache lookups in `vertex` stay on the GPU thread anyway (#629).

### The reuse table

- **What the lookup reads.** `PipelineRegs` lists every register field `vk_pipeline_cache.cpp` reads for a graphics lookup (`pipeline_regs.cpp`): a few hundred words of the context, uconfig and SH registers. The user data is not in it: through it the lookup reads only the sharps and the fetch shader, which are checked at every draw (below). A field the lookup starts to read has to be added to the list; `--check-pipeline-reuse` catches one that is missing.
- **Register changes.** Liverpool calls `PipelineRegTracker::OnWrite` before it copies the words of every `SET_CONTEXT_REG`, `SET_SH_REG`, `SET_UCONFIG_REG` and `SET_CONFIG_REG` (graphics and compute rings) and `OnReset` at `CLEAR_STATE`. A changed value of a listed register advances the tracker's generation. The draw packets' own writes (index base and type, the number of indices and instances) go past the tracker; the lookup does not read them. `LOAD_*_REG`, which loads registers from memory, is not implemented; when it is, it has to call `OnWrite` with the loaded words. The vertex and pixel shader's program registers and the primitive type change between most draws and are left out of the generation.
- **States.** When the generation changed since the last draw, `PipelineRegSnapshots` copies the listed registers (without the per-draw ones) and finds an equal earlier copy, or keeps this one, and returns its id. Equal means equal in every word, not only in a hash, so ids never confuse two states. States come back every frame, so a state seen once keeps its id.
- **Entries.** The reuse table is keyed by the state's id, the per-draw registers and the indirect draw parameters, and holds the pipeline, its key (for the wait on a pipeline a worker still creates) and for each bound stage the program, its hash and the specialization and fetch shader words of the lookup that made the entry.
- **A reuse.** For each stage the GPU thread reads the program's hash from its binary info, refreshes the program's user data and flattened user data as the lookup does, and checks the sharps and the fetch shader with `StageSpecialization::MatchesSharps`, the part of `Matches` that does not compare the runtime info and the first binding (those come from registers, which are the same). Any difference, and the draw takes the full lookup as before, which then replaces the entry. Correctness never depends on the table: what it skips is computed from registers whose values are the same as when the entry was made. A lookup that translated a shader makes no entry: translation advances the next stage's first binding by each image's bindings, while a lookup of a known permutation advances it by one per image (`Info::AddBindings`), so the next identical draw can pick another vertex permutation. They differ for storage images with mip fallback, on devices without image load and store with LOD.
- **Scope.** Only draws with the vertex and pixel stages reuse (`stage_enable` is `Vs`): the other stages read guest memory beyond the sharps (the tessellation constants, the geometry stage's copy shader). Devices without dynamic vertex input keep the vertex formats in the key and do not reuse either. Compute pipelines are looked up as before.
- **Limits.** 1024 states and 16384 entries; past either both tables start over. The listed fields are whole, so render target addresses are part of a state: targets that move from frame to frame make new states, which only lowers the reuse rate.
- **Switches.** `bb-run --no-pipeline-reuse` looks up every draw, for comparison runs. `bb-run --check-pipeline-reuse` takes the full lookup after every reuse as well and stops the run when it finds another pipeline. The frame profile counts reuses (`reuse`, including the rare reuse whose draw is skipped while a worker creates its pipeline) and full graphics lookups (`lookup`); the time of a failed reuse check counts only in `pipeline`.

The tracker costs a compare per register word written. The full lookup is about 0.9 µs a draw in `play-walk`, a reuse about 0.4 µs, so the expected gain was about 0.5 µs on each of 1500–2000 draws, 0.7–1 ms of the GPU thread's frame.

### Measured

A first version took every context register into the snapshot. Context registers change between about 40 % of the draws (depth bounds, scissors, viewports, render target addresses), and copying and comparing 10 KB of them each time made `pipeline` slower than before (2.4 ms in `play-walk`). Only the registers the lookup reads changed that: a state changes much less often and its copy is a few hundred words.

- **Reuse rate.** `--check-pipeline-reuse --log profile=debug` at 120: 96 % of the graphics lookups in `yharnam-route` `play-walk`, 97 % in `play-sprint`, 96 % in `play-attack`; 97 %, 94 % and 96 % in `yharnam-fight` `play-walk`, `play-fight-strong` and `play-heal-again`; 98 % in `clinic-fight` `play-walk` at 30. The check found no draw whose full lookup took another pipeline in these three runs.
- **`pipeline` part.** In `yharnam-route` `play-walk` at 120 the part fell to 1.10 ms for about 2050 lookups (0.53 µs each); a full lookup costs 0.88 µs. A reuse costs 0.39 µs: reading the binary info, the flattened user data and the sharp checks of two stages.
- **GPU thread CPU per present** (`thread_cpu.py`, `relwithdebinfo`, 120 FPS, three runs per side on a quiet machine, interleaved, a segment dropped where the load from other programs went over 4 cores; ms, median and the runs):

| segment | `main` | this change |
|---|---|---|
| `yharnam-route` `play-stand` | 6.19 (6.15–6.94) | 5.93 (5.89–6.57) |
| `yharnam-route` `play-walk` | 8.35 (8.28, 8.42) | 7.99 (7.97, 8.01) |
| `yharnam-route` `play-camera` | 6.47 (6.46–6.56) | 6.23 (6.22, 6.25) |
| `yharnam-route` `play-walk-again` | 5.53 (5.36–5.56) | 5.46 (5.25–6.22) |
| `yharnam-fight` `play-walk` | 8.49 (8.28–8.77) | 8.22 (7.92–9.14) |

In the segments the route repeats, the GPU thread gets 0.25–0.36 ms cheaper per present, beyond the spread in `play-walk` (108 to 111 FPS) and `play-camera`. That is about half the estimate: the reuse itself still costs 0.4 µs a draw, and the tracker and the resolves add some. The later segments of both routes go where the hunter's dodges, attacks and the enemies take it, and runs of the same build differ there by 1–5 ms per present, so they decide nothing either way. The GPU thread is still the bound at 120 in `play-walk`; the next cut is #779.

## A two-stage GPU thread (#779)

The idea: split the GPU thread into a strict chain of two threads. Stage A parses PM4, keeps the registers and looks up the pipeline. Stage B alone owns the buffer cache, the texture cache and the page manager: lookups, uploads, bindings, barriers, then the recording that the recording thread already replays. The caches keep one owner and need no locks.

**Decision: not a milestone now.** After #780 the GPU itself bounds the heaviest segments at 120, not the GPU thread. Splitting the thread would take its CPU time per present from about 8 ms to about 6 ms, but the frames would still wait for the GPU, which is busy 95–99 % of the time there at nearly its top clock. In `yharnam-route` `play-walk` that caps the gain at about 3 % (111 to at most 114 FPS), for a change of about 2500 lines of Core code with a new class of ordering bugs. R13's next step is the GPU's own work per frame (#827). The split is worth reassessing once the GPU needs clearly less time per frame than the GPU thread and the GPU thread is still above 8.33 ms.

### Measured split

`relwithdebinfo` build of `main` at `f8861cb2` (after #780) with a local probe that counts packet types per frame, `--window`, warm cache, 120 FPS. Two `--log profile=debug` runs of `yharnam-route` and one of `yharnam-fight` for the counters; one run each without counters for the thread times; the GPU's busy share and clock (`hwmon` `freq1_input`) sampled every 0.1 s beside them. Load from other programs averaged 1.4–2.9 cores during these runs (another session was building), so their thread times read slightly high; the #780 runs on a quiet machine are given beside them.

Stage A is `commands` minus `draw` (PM4 processing outside the draws) plus `pipeline`; stage B is the rest of `draw` (`bind`, `render`, `vertex`, `state` and the unscoped rest: render state, barriers, the draw record). Counters, a normal frame, ms:

| route | segment | `commands` | A | B | A share |
|---|---|---|---|---|---|
| yharnam-route | play-walk | 7.89, 8.86 | 2.13, 2.27 | 5.76, 6.58 | 26–27 % |
| yharnam-route | play-sprint | 9.23, 9.60 | 2.45, 2.47 | 6.78, 7.13 | 26 % |
| yharnam-route | play-dodge | 10.98, 10.32 | 2.87, 2.67 | 8.11, 7.65 | 26 % |
| yharnam-route | play-attack | 10.52, 10.39 | 2.80, 2.76 | 7.72, 7.63 | 27 % |
| yharnam-fight | play-walk | 9.53 | 2.41 | 7.12 | 25 % |
| yharnam-fight | play-heal | 9.87 | 2.68 | 7.19 | 27 % |

#780 took `pipeline` from 1.48 to about 1.0–1.2 ms, so stage A fell from 27–29 % of `commands` (#654) to 25–27 %. The counters read about 10 % above the thread's CPU time (their own scopes, `docs/test-rig.md`). Applied to the GPU thread's CPU per present (7.97–8.01 ms in `play-walk` in the quiet #780 runs), stage B would take about 5.9 ms, plus the cost of handing over about 1500 draw records and about 2000 forwarded packets a frame (register writes not counted) (estimated 0.2–0.4 ms, as for the deferred command buffer). Stage B would fit the 8.33 ms slot in every segment.

The GPU does not. Per present (`thread_cpu.py`, runs without counters):

| route | segment | FPS | GPU thread CPU | GPU | GPU busy | GPU clock (MHz) |
|---|---|---|---|---|---|---|
| yharnam-route | play-walk | 107, 101 (#780: 111) | 8.28, 8.85 (#780: 7.97–8.01) | 8.88, 9.71 (#780: 8.74–8.77) | 95, 99 % | 2502, 2577 |
| yharnam-route | play-sprint | 106, 109 | 8.49, 8.27 | 9.15, 9.01 | 97, 98 % | 2574, 2570 |
| yharnam-route | play-dodge | 102, 86 | 8.89, 10.39 | 9.54, 10.26 | 97, 88 % | 2584, 2314 |
| yharnam-fight | play-walk | 108 (#780: 2 runs) | 8.53 (#780: 8.13, 8.31) | 9.26 (#780: 8.71, 8.86) | 99 % | 2548 |
| yharnam-fight | play-heal | 108 | 8.61 | 8.94 | 97 % | 2558 |

The highest clock seen in these runs was 2631 MHz, and the heavy segments run at 2500–2585 MHz: the GPU is not waiting for work at a low clock, as it does in the clinic (`docs/test-rig.md`, "Heavy areas at 30, 60 and 120"). In each heavy segment the GPU needs about as long per present as the frame interval, and longer than the GPU thread's CPU time. A GPU thread that cost nothing would raise `play-walk` to 1000 / 8.75 = 114 FPS at most.

### Where stage A would wait for stage B

Per frame in `yharnam-route` `play-walk` (probe counts, both runs): 1330–1500 draws, 3 indirect draws, 110–130 dispatches on the graphics ring and none on the compute rings, 220–265 `WAIT_REG_MEM`, 420–475 end-of-pipe events, 2 end-of-shader events, 1150–1270 `DMA_DATA`, 44 `WRITE_DATA`, 44 `WAIT_ON_CE_COUNTER`, 4–6 occlusion counter dumps, no `COND_EXEC`, no `MEM_SEMAPHORE` wait, no GDS read with `Finish`, no `Finish` from other paths and no callback from a guest thread. The other segments and `yharnam-fight` have the same mix.

- **Guest-visible writes** (end-of-pipe and end-of-shader fences and their interrupts, `RELEASE_MEM`, `WRITE_DATA`, the occlusion counter dump, `STRMOUT_BUFFER_UPDATE`, `DUMP_CONST_RAM`): the guest reads them as "the GPU is done with the work before". They have to run on stage B in command order, after B has read what the draws before them use, as they now run after `WaitCopies`. Stage A forwards them and does not wait. The guest sees them later by stage B's lag, at most the queued part of a frame.
- **`WAIT_REG_MEM`** is the real cost. Today all 220–265 a frame pass at the first test (no spin at all in the probe), and with twice as many end-of-pipe events most of them presumably wait for a label that an earlier event of the same stream wrote. Once stage B writes the labels, each such wait would stop stage A until B has caught up, about every six draws, and the two stages would run nearly in series. Stage A would need a table of the label writes it has forwarded and B has not done yet, and test a wait on such an address against the forwarded value: B runs in order, so everything after the wait comes after the write. Only a wait on memory the guest or another queue writes polls as today. The first step would have to confirm by a probe which labels the waits read.
- **The constant engine.** 44 `WAIT_ON_CE_COUNTER` a frame: the draw engine waits for the constant engine's dumps, which hold the descriptors the draws read. Stage B reads them when it binds, so the draw engine's counter (`INCREMENT_DE_COUNTER`) has to count on B, and the constant engine's `WAIT_ON_DE_COUNTER_DIFF` then waits for B. That is a bounded lag (the ring's depth), not a drain.
- **Packets that read guest memory.** Indirect draws and dispatches read their arguments on the GPU through the buffer cache: stage B, nothing for A. `DMA_DATA` fills and copies go through the buffer cache too, and the GPU's copies do not write guest memory (the range is marked GPU-modified), so stage A reads the same guest memory as the GPU thread does today. `COND_EXEC` reads memory a GPU write may have set and would drain; the game does not use it here. `SET_PREDICATION` and `LOAD_*_REG` are not implemented; a `LOAD_*_REG` from GPU-written memory would drain.
- **The pipeline lookup reads guest memory** (the sharps, the fetch shader, the SRT tables of the flattened user data), and stage B reads the same memory again when it binds. The two reads must agree, so the record carries what A read, and B binds from it. That makes the records larger than a register delta.
- **Synchronous paths** (`Finish` at a GDS read, `FlushGpuModified`, downloads, the texture cache's collection) belong to stage B. None ran in these segments.

### Write faults

A guest thread's write fault on a page only the CPU wrote is handled on that thread (`MarkCpuModifiedFromFault`) and touches neither stage. A fault on a page the GPU wrote, an invalidation and an unmap with images in it go through `OnGpuThread`: a callback the GPU thread runs between two packets while the guest thread waits. They would go to stage B's queue, which B runs between two records, so the guest thread still waits for at most one draw's work. Stage A's queued records hold guest addresses, not cache objects, and B resolves them after the callback, in the same order as today. In these segments no callback ran; the 4–5 ms of `fault` per frame are the fast path on the guest threads, which #809 targets.

### Size and steps, if it is reassessed

1. **Records on one thread** (Core). Stage B gets its own register file, fed by the register writes stage A forwards with each draw, and the draw record carries A's pipeline, its sharps and user data. Still one thread; measured as the record cost on the GPU thread (`thread_cpu.py`), with a check mode that compares B's draws against today's.
2. **Fences, labels and the constant engine on B, the label table on A** (Core). Measured as the `WAIT_REG_MEM` waits that the table does not answer.
3. **The second thread** (Core), the callback queue on B and a switch that runs both stages on one thread for comparison runs. Measured at 120 against `main`: FPS, both threads' CPU per present and the GPU's time per present in `yharnam-route` and `yharnam-fight`; p99 at 60 in `yharnam-route`.

Each step is one PR near the 800-line limit, about 2500 lines in all.

### Rejected ideas

- **Moving guest memory back to VRAM.** It does not apply: the buffer cache always copies guest buffers into its device-local arena (`arena_memory_type_index`), and the texture cache allocates its images with a device-local preference, so the GPU never reads guest memory over the bus.
- **Skipping draws while a pipeline builds**, beyond the 8 ms budget a frame already waits (#521). It costs picture quality for a case the warm disk cache (R8.1) makes rare; the measured segments run with a warm cache.
