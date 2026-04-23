# RingBuffer_PackedState

[![GitHub stars](https://img.shields.io/github/stars/pxq-dk/RingBuffer-Cortex?style=social)](https://github.com/pxq-dk/RingBuffer-Cortex/stargazers)

ISR-safe, DMA-friendly ring buffer for ARM Cortex-M — LDRH/STRH per field, policy-based IRQ protection, lock-free SPSC, compile-time unit tests.

---

## Features

- **Per-field halfword access** — head and tail are stored as adjacent `uint16_t` fields. Reads are `LDRH`, writes are `STRH`. Each side in SPSC touches only its own halfword with no read-modify-write of the other.
- **Lock-free SPSC** — with `Topology::SPSC<>`, the producer writes head (`STRH`) and the consumer writes tail (`STRH`) without any IRQ masking. For example, an ISR pushes and main pops — or vice versa. The two stores target different halfword addresses and never collide.
- **Policy-based IRQ protection** — choose `NoIrqProtection`, `SpscProtection`, or `IrqProtection` as a template argument. Zero overhead when protection is not needed.
- **Conditional `volatile`** — `state` and `buffer` are automatically `volatile` when `needs_volatile` is true, ensuring the compiler always generates actual loads and stores across ISR boundaries.
- **No software division** — power-of-2 sizes use `& (Size-1)`; non-power-of-2 sizes use compare-and-subtract. Both avoid `__aeabi_uidivmod` on Cortex-M0+.
- **DMA-friendly contiguous area API** — zero-copy access via `get_contiguous_push_area` / `get_contiguous_pop_area` for direct DMA transfers.
- **Compile-time unit tests** — a `static_assert` in the constructor runs a full test suite at compile time. A broken instantiation will not compile.
- **Header-only** — single `.h` file, no dependencies beyond the C++ standard library.

---

## Requirements

- C++17 or later
- ARM Cortex-M target (GCC `arm-none-eabi`) or any C++17 compiler for host-side use
- `__disable_irq()`, `__get_PRIMASK()`, and `__set_PRIMASK()` available when using the built-in `IrqProtection` (standard CMSIS). Custom protection schemes (RTOS, custom critical sections) can be used by defining a custom policy — see IRQ Protection Policies.

---

## Usage

### Basic — no IRQ protection

```cpp
#include "RingBuffer_PackedState.h"

RingBuffer_PackedState<uint8_t, 32> rb;  // defaults to IrqProtection::None

rb.push(42);

uint8_t val;
if (rb.pop(val)) {
    // val == 42
}
```

### ISR-safe — with IRQ protection

```cpp
RingBuffer_PackedState<uint8_t, 32, Topology::MPMC> rb;

// Safe to call from both main context and ISR
rb.push(42);
```

### Lock-free SPSC — e.g. ISR producer, main consumer (or vice versa)

```cpp
// SPSC: volatile fields (real LDRH/STRH) but no IRQ masking.
// ISR owns head, main owns tail — their STRH writes never collide.
// The roles can be reversed: main as producer, ISR as consumer.
RingBuffer_PackedState<uint8_t, 32, Topology::SPSC<>> rb;

// In ISR (producer):
rb.push(byte_from_peripheral);

// In main (consumer):
uint8_t val;
if (rb.pop(val)) {
    // process val
}
```

### DMA — contiguous area API

Two variants are available depending on when the head/tail index should advance.

> **Note:** `get_contiguous_push_area` and `get_contiguous_pop_area` read state without a Guard. Only one producer and one consumer may use these functions at a time (SPSC — Single Producer, Single Consumer).


**Commit variant** — write/read first, then advance index:
```cpp
RingBuffer_PackedState<uint8_t, 64, IrqProtection> rb;

// Get pointer and count for DMA write
auto area = rb.get_contiguous_push_area(32);
// Configure DMA: area.ptr, area.count
// ...wait for DMA complete...
rb.commit_push(area.count);    // advance head after DMA completes

// Get pointer and count for DMA read
auto out = rb.get_contiguous_pop_area(32);
// Configure DMA: out.ptr, out.count
// Note: if data wraps the buffer boundary, call twice
// ...wait for DMA complete...
rb.commit_pop(out.count);      // advance tail after DMA completes
```

**Reserve variant** — advance index immediately, write/read after:
```cpp
auto area = rb.reserve_push(32);  // head advances immediately
// Configure DMA: area.ptr, area.count
// ...wait for DMA complete...
// No commit needed — index already advanced

auto out = rb.reserve_pop(32);    // tail advances immediately
// Configure DMA: out.ptr, out.count
// ...wait for DMA complete...
// No commit needed — index already advanced
```

---

## Template Parameters

| Parameter | Default | Description |
|---|---|---|
| `T` | — | Element type |
| `Size` | — | Total buffer slots. Effective capacity is `Size-1`. Must be in range [2, 65535]. |
| `IrqPolicy` | `Topology::None` | IRQ protection policy. See IRQ Protection Policies below. |

---

## API Reference

| Function | Description |
|---|---|
| `push(item)` | Push item. Returns `false` if full. Guard-protected. |
| `pop(item)` | Pop item. Returns `false` if empty. Guard-protected. |
| `isFull()` | Returns `true` if buffer is full. |
| `isEmpty()` | Returns `true` if buffer is empty. |
| `getCount()` | Returns number of elements currently in buffer. |
| `get_contiguous_push_area(max)` | Returns pointer + count of contiguous writable slots. Call `commit_push()` after writing. |
| `commit_push(count)` | Advances head by `count` after a contiguous push. Clamped to available space. |
| `get_contiguous_pop_area(max)` | Returns pointer + count of contiguous readable slots. Call `commit_pop()` after reading. |
| `commit_pop(count)` | Advances tail by `count` after a contiguous pop. Clamped to available space. |
| `reserve_push(max)` | Advances head immediately, returns pointer + count. Write after the call — no commit needed. |
| `reserve_pop(max)` | Advances tail immediately, returns pointer + count. Read after the call — no commit needed. |

---

## IRQ Protection Policies

All built-in policies are nested types inside `IrqProtection`:

All built-in topologies use `PrimaskLock` by default. Pass a custom lock implementation as a template argument to the chosen topology to swap it out.

| Policy | `needs_volatile` | `lock_p_needed` | `lock_c_needed` | Use case |
|---|---|---|---|---|
| `Topology::None` | `false` | `false` | `false` | Single context — no concurrency |
| `Topology::SPSC` | `true` | `false` | `false` | Lock-free SPSC — e.g. ISR owns head, main owns tail (or vice versa) |
| `Topology::MPSC` | `true` | `true` | `false` | Multiple producers, single consumer — `push` guarded, `pop` free |
| `Topology::SPMC` | `true` | `false` | `true` | Single producer, multiple consumers — `pop` guarded, `push` free |
| `Topology::MPMC` | `true` | `true` | `true` | Multiple producers and consumers — both sides guarded |

`lock_p_needed` activates a `ProducerGuard` (PRIMASK save/restore) around `push`, `commit_push`, and `reserve_push`. `lock_c_needed` does the same for `pop`, `commit_pop`, and `reserve_pop`. This means `MPSC` pays no guard overhead on the consumer side, and `SPMC` pays none on the producer side.

**Custom lock implementation** — define a struct with `lock()` / `unlock(uint32_t)` and pass it directly to the chosen topology. Example using FreeRTOS:

```cpp
struct FreeRtosLock {
    static uint32_t lock()           { taskENTER_CRITICAL(); return 0; }
    static void     unlock(uint32_t) { taskEXIT_CRITICAL(); }
};

RingBuffer_PackedState<uint8_t, 32, Topology::MPMC<FreeRtosLock>> rb;
```

---

## Performance (STM32G051, Cortex-M0+, `-Os`)

- **`push` / `pop`**: ~12 instructions, no stack spills, no out-of-line calls
- **`IrqProtection::SPSC` overhead**: none — no IRQ masking, `volatile` fields ensure actual LDRH/STRH
- **`IrqProtection::MPSC`**: guard on `push` only (~7–9 cycles); `pop` is free
- **`IrqProtection::SPMC`**: guard on `pop` only (~7–9 cycles); `push` is free
- **`IrqProtection::MPMC` overhead**: `MRS` + `CPSID` + `MSR` on both `push` and `pop` (~7–9 extra cycles each)
- **Power-of-2 size**: index wrapping uses a single `AND` instruction (~2 cycles)
- **Non-power-of-2 size**: index wrapping uses compare-and-subtract (~4 cycles) — no software division

---

## Design Notes

**Why adjacent `uint16_t` fields instead of a packed `uint32_t`?**

Storing head and tail as separate `uint16_t` fields in a 4-byte-aligned struct means each field is written with a single `STRH` instruction that touches only its own halfword. On Cortex-M0+, `STRH` to SRAM is not a read-modify-write — the bus writes exactly the addressed 16-bit location and leaves the adjacent halfword untouched.

This makes SPSC lock-free: the ISR writes `state.head` (STRH at offset 0) while main writes `state.tail` (STRH at offset 2) — or vice versa. The two stores target different bus addresses and cannot interfere, so no IRQ masking is needed.

An earlier version packed head and tail into a single `uint32_t` to give an atomic LDR snapshot of both fields. The trade-off: even though `push()` only changes head and `pop()` only changes tail, both had to do a full 32-bit read-modify-write (`LDR` + `STR`) to preserve the other field — which made lock-free SPSC impossible (a concurrent STR from the ISR could overwrite the stale field read by main).

**Why `Topology::SPSC<>` instead of `Topology::None<>` for SPSC?**

`Topology::SPSC<>` sets `needs_volatile = true`, which makes `state.head` and `state.tail` `volatile`. Without `volatile`, the compiler is free to cache a field value in a register and never re-read it — ISR writes to `state.head` would be invisible to main (or vice versa). `volatile` forces an actual `LDRH` on every access, at no runtime cost beyond the instruction itself. The Guard is still a no-op, so there is no IRQ masking overhead.

**Why waste one slot?**
Distinguishing full from empty without a separate count variable. If `head == tail` the buffer is empty; if `nextIndex(head) == tail` it is full. Simple, branchless, and correct.

**Why compile-time unit tests?**
A `static_assert` in the constructor runs `unit_test_ringbuffer::run_test()` at compile time against a `Size=4` test instance. If any test fails, the translation unit will not compile — no separate test binary required.

---

## License

Copyright (c) 2026 PxQ Technologies — https://pxq.dk

**Open Source / Non-Commercial Use:**
Free to use, modify, and distribute under the MIT License, provided it is used solely in open source or non-commercial projects. The copyright notice must be retained.

**Commercial Use:**
Requires either a written commercial license agreement with PxQ Technologies, or direct delivery by Erik Nørskov (PxQ Technologies) as part of a paid engagement or employment, in which case a license is granted for use within that specific project scope only.

Contact: https://pxq.dk

If you find this library useful, please consider giving it a ⭐ on [GitHub](https://github.com/pxq-dk/RingBuffer-Cortex) — it helps others discover it.
