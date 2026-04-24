# RingBuffer-Cortex

[![GitHub stars](https://img.shields.io/github/stars/pxq-dk/RingBuffer-Cortex?style=social)](https://github.com/pxq-dk/RingBuffer-Cortex/stargazers)

A ring buffer optimised for ARM Cortex-M microcontrollers, with lock-free SPSC support and policy-based IRQ protection.

---

## Why another ring buffer?

Most Cortex-M ring buffers use word-sized (`size_t` / `uint32_t`) indices with `std::atomic` or plain `volatile`, and either require power-of-2 sizes or silently call `__aeabi_uidivmod` on wrap. This one takes a different set of micro-architectural decisions:

- **Lock-free SPSC without `std::atomic`** — head and tail are packed as two `uint16_t` fields in a 4-byte-aligned struct. The producer `STRH` at offset 0 and the consumer `STRH` at offset 2 target different bus addresses — they cannot collide. No libatomic calls, no fences.
- **Single-LDR state snapshot** — both indices read in one 32-bit bus transaction (`readHT()`). Other libraries do two separate reads or rely on atomic word-sized indices.
- **Topology-selectable IRQ protection** — `None` / `SPSC` / `MPSC` / `SPMC` / `MPMC` chosen per-instance, with producer and consumer guards independent (MPSC pays no guard on `pop`, SPMC pays none on `push`).
- **No software division on any path** — power-of-2 sizes use `AND`, non-power-of-2 use compare-and-subtract. No `__aeabi_uidivmod` ever emitted.
- **Compile-time unit tests** — `static_assert` in the constructor runs a full test suite at instantiation. A broken build will not compile.

---

## Quick Start

```cpp
#include "RingBuffer_PackedState/RingBuffer_PackedState.h"

// Basic use — no IRQ protection
RingBuffer_PackedState<uint8_t, 32> rb;

rb.push(42);

uint8_t val;
if (rb.pop(val)) {
    // val == 42
}

// ISR-safe — PRIMASK save/restore, multiple producers/consumers
RingBuffer_PackedState<uint8_t, 32, Topology::MPMC> rb_mpmc;

// Lock-free SPSC — e.g. ISR producer + main consumer, or vice versa, no IRQ masking needed
RingBuffer_PackedState<uint8_t, 32, Topology::SPSC<>> rb_spsc;

// Custom lock — e.g. FreeRTOS critical section
RingBuffer_PackedState<uint8_t, 32, Topology::MPMC<FreeRtosLock>> rb_rtos;
```

See [`RingBuffer_PackedState`](RingBuffer_PackedState/) for the full API including DMA contiguous area access.

---

## Available

### [`RingBuffer_PackedState`](RingBuffer_PackedState/)

ISR-safe, DMA-friendly ring buffer for ARM Cortex-M. Head and tail stored as adjacent `uint16_t` fields — reads are `LDRH`, writes are `STRH`. Each side in SPSC owns one field exclusively, enabling lock-free operation without IRQ masking.

---

## Features

- **Topology-based policy** — `Topology::None<>` / `SPSC<>` / `MPSC<>` / `SPMC<>` / `MPMC<>`; producer and consumer guards are independent, so MPSC pays no overhead on `pop` and SPMC pays none on `push`; swap `PrimaskLock` for any custom lock (e.g. FreeRTOS) via `Topology::MPMC<FreeRtosLock>`
- **Compile-time unit tests** — a `static_assert` in the constructor verifies correctness at build time
- **Header-only** — single `.h` file per implementation, no dependencies beyond the C++ standard library
- **C++20** or later required

---

## License

Copyright (c) 2026 PxQ Technologies — https://pxq.dk

**Open Source / Non-Commercial Use:** MIT License — free to use, modify, and distribute in open source or non-commercial projects. Copyright notice must be retained.

**Commercial Use:** Requires a written commercial license agreement with PxQ Technologies, or direct delivery by Erik Nørskov (PxQ Technologies) as part of a paid engagement, in which case a license is granted for that specific project scope.

Contact: https://pxq.dk

If you find this library useful, please consider giving it a ⭐ on [GitHub](https://github.com/pxq-dk/RingBuffer-Cortex) — it helps others discover it.
