# RingBuffer-Cortex

[![GitHub stars](https://img.shields.io/github/stars/pxq-dk/RingBuffer-Cortex?style=social)](https://github.com/pxq-dk/RingBuffer-Cortex/stargazers)

A ring buffer optimised for ARM Cortex-M microcontrollers, with lock-free SPSC support and policy-based IRQ protection.

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
- **C++17** or later required

---

## License

Copyright (c) 2026 PxQ Technologies — https://pxq.dk

**Open Source / Non-Commercial Use:** MIT License — free to use, modify, and distribute in open source or non-commercial projects. Copyright notice must be retained.

**Commercial Use:** Requires a written commercial license agreement with PxQ Technologies, or direct delivery by Erik Nørskov (PxQ Technologies) as part of a paid engagement, in which case a license is granted for that specific project scope.

Contact: https://pxq.dk

If you find this library useful, please consider giving it a ⭐ on [GitHub](https://github.com/pxq-dk/RingBuffer-Cortex) — it helps others discover it.
