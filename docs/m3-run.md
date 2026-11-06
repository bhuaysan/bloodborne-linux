# M3 run: the game at its main loop

State at the end of M3 (#106). `bb-run --stop-at-main-loop` exits 0 after about 0.9 s, under `relwithdebinfo` and `debug`; the game test is `BbRunGame.ReachesTheMainLoop`.

## What it took

Three blockers stood between the entry and the main loop. All three come from the PS4 keeping addresses in 40 bits:

- **Thread handles.** The eboot's mutex (0x20368c0) keeps its owner's handle masked to 40 bits and compares it with the full handle, so a host heap pointer (0x7f…) as handle made the main thread wait for itself. Thread objects now come from a pool below 2 GiB (design note, Threads).
- **Compute queue ids.** libSceGnmDriver `29oKvKXzEZo` (map a compute queue) must return a positive id; with 0 the render thread's device creation fails and the main thread dereferences a null device context (0x2167870). It now hands out ids from 1.
- **Guest addresses.** The game puts direct-memory addresses into 40-bit GPU fields, so a mapping at 0x7f… came back cut to 0xff… (#214). Guest mappings now lie in a window below 2^40 (design note, Memory).

## What runs after the main loop is reached

Without `--stop-at-main-loop` the game keeps going: about 50 threads (job workers, FMOD, the render thread, network and trophy threads), the main thread spins in the frame pacer (0x2034770), and the render thread maps its compute queue, submits command buffers and flips three times in the first second. Then it waits on a condition variable; nothing executes the GPU work yet (M4).

## Imports still stubbed

Calls in the first 25 s of a `relwithdebinfo` run (`bb-run --timeout 25`), from the stub log. The stub log reports calls 1 to 10 and then every 1000th, so the counts are lower bounds: 10 means 10 to 999 calls, 1000 means 1000 to 1999.

At the moment the main loop is reached, only two stubs have been called: `sceCommonDialogInitialize` and libkernel `p5EcQeEeJAE` (once each).

| Calls | Library | NID | Name |
|---|---|---|---|
| 513 933 000 | libSceAudioOut | `QOQtbeDqsT4` | sceAudioOutOutput |
| 452 163 000 | libSceNet | `drjIbDbA7UQ` | sceNetEpollWait |
| 9000 | libSceGnmDriver | `jg33rEKLfVs` | unknown |
| 4000 | libSceGnmDriver | `gAhCn6UiU4Y` | unknown |
| 4000 | libSceGnmDriver | `bQVd5YzCal0` | unknown |
| 3000 | libSceGnmDriver | `KXltnCwEJHQ` | unknown |
| 1000 | libSceNpManager | `3Zl8BePTh9Y` | sceNpCheckCallback |
| 1000 | libSceNetCtl | `iQw3iQPhvUQ` | sceNetCtlCheckCallback |
| 1000 | libSceHttp | `qISjDHrxONc` | sceHttpWaitRequest |
| 1000 | libSceGnmDriver | `0H2vBYbTLHI` | unknown |
| 10 | libScePad | `YndgXqQVV7c` | scePadReadState |
| 10 | libScePad | `yFVnOdGxvZY` | unknown |
| 10 | libSceGnmDriver | `W1Etj-jlW7Y` | unknown |
| 10 | libSceGnmDriver | `bX5IbRvECXk` | unknown (ring write offset, `docs/formats/gnm.md`) |
| 10 | libSceGnmDriver | `7qZVNgEu+SY` | unknown |
| 10 | libSceAjm | `AxoDrINp4J8` | sceAjmInstanceCreate |
| 4 | libSceAjm | `ElslOCpOIns` | unknown |
| 4 | libSceAjm | `dmDybN--Fn8` | unknown |
| 2 | libSceNetCtl | `JO4yuTuMoKI` | unknown |
| 2 | libSceAudioOut | `ekNvsT22rsY` | sceAudioOutOpen |
| 2 | libSceAjm | `-qLsfDAywIY` | sceAjmBatchWait |
| 2 | libSceAjm | `fFFkk0xfGWs` | sceAjmBatchStartBuffer |
| 1 | libScePad | `xk0AcarP3V4` | scePadOpen |
| 1 | libScePad | `hv1luiJrqQM` | scePadInit |
| 1 | libSceNpTrophy | `XbkjbobZlCY` | sceNpTrophyCreateContext |
| 1 | libSceNpTrophy | `TJCAxto9SEU` | sceNpTrophyRegisterContext |
| 1 | libSceNpTrophy | `q7U6tEAQf7c` | sceNpTrophyCreateHandle |
| 1 | libSceNpMatching2 | `10t3e5+JPnU` | sceNpMatching2Initialize |
| 1 | libSceNpManager | `VfRSmPmj8Q8` | sceNpRegisterStateCallback |
| 1 | libSceNpManager | `uFJpaKNBAj4` | unknown |
| 1 | libSceNpManager | `Ec63y59l9tw` | unknown |
| 1 | libSceNet | `SF47kB2MNTo` | sceNetEpollCreate |
| 1 | libSceNetCtl | `UJ+Z7Q+4ck0` | sceNetCtlRegisterCallback |
| 1 | libSceHttp | `f42K37mm5RM` | unknown |
| 1 | libSceGnmDriver | `nF6bFRUBRAU` | unknown |
| 1 | libSceGnmDriver | `ln33zjBrfjk` | unknown |
| 1 | libSceCommonDialog | `uoUpLGNkygk` | sceCommonDialogInitialize |
| 1 | libSceAudioOut | `JfEPXVxhFqA` | sceAudioOutInit |
| 1 | libSceAudioOut | `b+uAV89IlxE` | sceAudioOutSetVolume |
| 1 | libSceAjm | `RbLbuKv8zho` | sceAjmInstanceDestroy |
| 1 | libSceAjm | `Q3dyFuwGn64` | sceAjmModuleRegister |
| 1 | libSceAjm | `dl+4eHSzUu4` | sceAjmInitialize |
| 1 | libkernel | `p5EcQeEeJAE` | unknown |

Names are those of `symbols/nids.csv` (hash-matched); "unknown" means no candidate matched yet.

Notes for M4 and M5:
- The two busiest stubs spin: the FMOD output thread calls `sceAudioOutOutput` without blocking (it should block until the port takes the buffer), and the network thread polls `sceNetEpollWait`, which returns at once. Both burn a core each; an audio output that paces and an epoll wait that honours its timeout fix that (M5, and the network module).
- The GNM stubs on the render thread are the M4 work: command buffer state, the ring write offset `bX5IbRvECXk`, and whatever the render thread waits for after the third flip.
