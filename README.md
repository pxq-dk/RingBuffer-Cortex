# RingBuffer-Cortex

[![GitHub stars](https://img.shields.io/github/stars/pxq-dk/RingBuffer-Cortex?style=social)](https://github.com/pxq-dk/RingBuffer-Cortex/stargazers)

A ring buffer optimised for ARM Cortex-M microcontrollers, designed around specific performance trade-offs for embedded systems.

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

// ISR-safe — PRIMASK save/restore, safe in both main context and ISR
RingBuffer_PackedState<uint8_t, 32, IrqProtection> rb_isr;
```

See [`RingBuffer_PackedState`](RingBuffer_PackedState/) for the full API including DMA contiguous area access.

---

## Available

### [`RingBuffer_PackedState`](RingBuffer_PackedState/)

ISR-safe, DMA-friendly ring buffer for ARM Cortex-M. Head and tail packed into a single `uint32_t` for atomic LDR/STR and minimal bus transactions. Best suited for systems that poll buffer state frequently or run heavy DMA in the background.

---

## Features

- **Policy-based IRQ protection** — `NoIrqProtection` or `IrqProtection` (PRIMASK save/restore, safe in both ISR and non-ISR contexts)
- **Compile-time unit tests** — a `static_assert` in the constructor verifies correctness at build time
- **Header-only** — single `.h` file per implementation, no dependencies beyond the C++ standard library
- **C++17** or later required

---

## License

Copyright (c) 2026 PxQ Technologies — https://pxq.dk

**Open Source / Non-Commercial Use:** MIT License — free to use, modify, and distribute in open source or non-commercial projects. Copyright notice must be retained.

**Commercial Use:** Requires a written commercial license agreement with PxQ Technologies, or direct delivery by Erik Nørskov (PxQ Technologies) as part of a paid engagement, in which case a license is granted for that specific project scope.

Contact: https://pxq.dk
