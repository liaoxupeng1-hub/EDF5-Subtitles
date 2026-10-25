# EDF5 playback identity and lifecycle evidence

Analysis date: 2026-10-25. Static PE bytes and objdump evidence only; no game execution, game writes, or runtime acceptance.

Target: `EDF5.exe`

SHA-256: `6C2E54FE48EAD4FDC80B1EC34733E52044BDA3DF8E2FC0AFC9E6F6A93F23F7F6`.

All addresses below are RVAs relative to the executable's loaded base. The preferred base observed in this file is `0x140000000`.

## Playback entry ABI

- `0x47C9D0`: `bool PlayVoice(void* manager, GameString* name, void* outPair)`.
- `0x47CBA0`: `bool PlaySurroundVoice(void* manager, GameString* name, void* spatial, void* outPair, int a5, unsigned a6)`.
- `0x477C50`: `void* CreateVoice(void* bank, const void* requestPair)`.
- `0x92B148`: `int PlaybackState(int playbackId)`.

These are recovered native boundaries, not original source declarations. Windows x64 calling convention applies. Both public voice entry points test and return AL as a Boolean. The old source's `void*` return declaration should be corrected to `bool` when rebuilding.

`PlayVoice` preserves the third input argument as R14. At `0x47CB77`–`0x47CB87` it passes it as RDX to `0x4777B0`; `PlaySurroundVoice` preserves the fourth argument as R14 and the spatial third argument as R12, then forwards them as RDX/R9 to `0x4779A0` at `0x47CD55`–`0x47CD68`.

## How the native playback object is created

The two lower paths `0x4777B0` and `0x4779A0` call `0x477C50`. That function dispatches to the normal-bank or alternate-bank creation path.

- Normal-bank creation `0x477D20`: obtains a playback ID from `0x92D808`, rejects `-1`, allocates a `0x70`-byte native object, writes the playback ID to object `+0x10` at `0x477E0C`, and returns that object.
- Alternate-bank creation `0x477E70`: obtains a playback ID, rejects `-1`, allocates the same-sized native object, writes the playback ID to object `+0x10` at `0x477FCE`, and returns it.

After `0x477C50` returns, the caller wraps the returned native pointer in the game's custom reference-counted pair. The wrapper control block is `{strongCount:u32, weakCount:u32, nativePointer:u64}`. An out pair is `{controlBlockPointer, nativePointer}`: `outPair + 0x08` is the native object. This is observable in `0x4778B4`–`0x4778E6` and the copy/lock routine `0x476AF0`.

The out pair is optional: `0x47793E` tests it for null. Capturing only the caller-provided out pair would therefore miss otherwise successful voice calls with no output pair.

## Safe recommended capture

Use a thread-local capture scope around the two public voice calls. Within that scope, a detour on `CreateVoice` calls the original, then immediately copies the returned native object's 32-bit field at `+0x10`. Retain only the integer ID, never the native pointer. This happens while the ordinary call chain owns the fresh object and before it is returned to the caller's reference-counted container.

Commit a captured subtitle only after the enclosing voice entry returns success. Keep capture scopes nestable so an inner voice request cannot replace the outer name or captured IDs. If a successful top-level call yields no captured ID, do not claim it is synchronized or invent a fixed normal duration.

This design covers optional null output pairs and avoids adding game references, mutating game pairs, or releasing game-owned objects from the render thread.

## Playback ID includes a slot token

The resolver at `0x9568DC`:

1. Rejects negative IDs and IDs with low bit zero.
2. Uses the upper 16 bits as a slot index and checks it against the slot count.
3. Uses a slot stride of `0x138`.
4. Compares the lower 16 bits of the ID with the token stored at slot `+0x38`.
5. Returns null if the token does not match.

Thus a saved integer is not merely an unchecked array index. The engine rejects a stale ID when its slot token differs. As with the engine's own users, eventual finite-token wraparound remains a theoretical limit; do not describe this as an unbounded unique identifier.

## End-state query

`0x92B148` acquires/releases the engine's audio synchronization around its internal query (`0x939AA4` / `0x939AC8`). The internal function `0x92B16C` resolves the ID through `0x9568DC` and returns `3` when no matching playback exists (`0x92B188`–`0x92B190`). It otherwise consults backend state and can follow associated child playbacks before returning.

Existing game code at `0x390BA2` reads `native + 0x10`, calls `0x92B148`, and treats a result other than `3` as still active. The rebuilt subtitle should use this same predicate instead of guessing durations or storing a native object pointer.

The exact human-readable meanings of result `1` and `2` were not proven in this pass. Use `state != 3` to reproduce the game's retained-playback condition; do not label those values as specifically playing, paused, or preparing without additional evidence. The native creation/playback call may include preparation before audible output; sample-accurate start alignment is not statically accepted here.

## Stop and replacement

Before replacing an output voice, `0x4777E5`–`0x477825` (and the analogous surround path) stops its prior playback via `0x92B4DC(id)`, resets native object ID to `-1`, and releases the old pair. `PlayVoice` has analogous cleanup at `0x47CA99` and `0x47CB20`.

The subtitle retains the old playback ID, so it can independently notice the stopped/released backend instance and fade the old caption, even if the caller reuses its pair for a new voice. Querying the pair's current native pointer each frame would incorrectly follow the replacement, so avoid that approach.

## Offline versus in-game boundary

Static analysis establishes the ABI, capture point, ID layout, resolver validation and end-state predicate. It does not establish audible onset latency, pause behavior across every menu, thread timing, retry teardown safety, all scripted/random voice coverage, or actual subtitle/voice synchronization. The candidate must be tested with short/long voices, overlapping voices, repeated IDs, stop/interruption, pause/resume, and retry.
