# RingBuffer-Cortex

A ring buffer optimised for ARM Cortex-M microcontrollers, designed around specific performance trade-offs for embedded systems.

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
