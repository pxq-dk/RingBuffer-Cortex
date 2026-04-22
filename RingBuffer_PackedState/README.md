# RingBuffer_PackedState

[![GitHub stars](https://img.shields.io/github/stars/pxq-dk/RingBuffer-Cortex?style=social)](https://github.com/pxq-dk/RingBuffer-Cortex/stargazers)

ISR-safe, DMA-friendly ring buffer for ARM Cortex-M — atomic LDR/STR, policy-based IRQ protection, compile-time unit tests.

---

## Features

- **Atomic state** — head and tail packed into a single `uint32_t` for atomic LDR/STR on Cortex-M0+. No separate counter variable.
- **Policy-based IRQ protection** — choose `NoIrqProtection` or `IrqProtection` as a template argument. Zero overhead when protection is not needed.
- **Conditional `volatile`** — `state` and `buffer` are automatically `volatile` when `IrqProtection` is used, ensuring the compiler never caches values across ISR boundaries.
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

RingBuffer_PackedState<uint8_t, 32> rb;

rb.push(42);

uint8_t val;
if (rb.pop(val)) {
    // val == 42
}
```

### ISR-safe — with IRQ protection

```cpp
RingBuffer_PackedState<uint8_t, 32, IrqProtection> rb;

// Safe to call from both main context and ISR
rb.push(42);
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
| `IrqPolicy` | `NoIrqProtection` | IRQ protection policy. Use `IrqProtection` for ISR-safe operation. |

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

| Policy | `needs_volatile` | Use case |
|---|---|---|
| `NoIrqProtection` | `false` | Single context only, or already inside a critical section |
| `IrqProtection` | `true` | Shared between main context and ISR |

Custom policies are supported — implement `needs_volatile`, `lock()` (returns `uint32_t` state), and `unlock(uint32_t)` (restores state). Example using FreeRTOS:

```cpp
struct RtosProtection {
    static constexpr bool needs_volatile = true;
    static uint32_t lock()             { taskENTER_CRITICAL(); return 0; }
    static void     unlock(uint32_t)   { taskEXIT_CRITICAL(); }
};

RingBuffer_PackedState<uint8_t, 32, RtosProtection> rb;
```

---

## Performance (STM32G051, Cortex-M0+, `-Os`)

- **`push` / `pop`**: ~13 instructions, no stack spills, no out-of-line calls
- **`IrqProtection` overhead**: `MRS` + `CPSID` + `MSR` per call (~7–9 extra cycles); saves and restores PRIMASK for correct nesting
- **Power-of-2 size**: index wrapping uses a single `AND` instruction (~2 cycles)
- **Non-power-of-2 size**: index wrapping uses compare-and-subtract (~4 cycles) — no software division

---

## Design Notes

**Why packed `uint32_t` state?**
On Cortex-M0+, a 32-bit aligned load (`LDR`) and store (`STR`) are atomic. Packing head and tail into a single `uint32_t` gives an atomic snapshot of both fields in one instruction, without needing a critical section for read-only operations like `isEmpty()`.

**PackedState trade-off vs. split head/tail:**
The packed approach has a cost on `push()` and `pop()`: even though `push()` only updates head and `pop()` only updates tail, both must perform a read-modify-write (`LDR` + `STR`) on the full 32-bit state to preserve the other field. A split design with separate head and tail variables would allow `push()` and `pop()` to each write only a single 4-byte variable (`STR` only) — saving 1 cycle per call.

However, the packed approach wins on state queries: `isEmpty()`, `isFull()`, and `getCount()` read both indices in a single `LDR` (1 cycle, 1 bus transaction) instead of two separate reads (2 cycles, 2 bus transactions).

On Cortex-M0+ the CPU and DMA share the same AHB bus. Halving the number of bus transactions for state queries directly reduces contention with DMA transfers — giving DMA more bandwidth under heavy load. This makes `RingBuffer_PackedState` particularly well suited to DMA-heavy systems where the buffer state is polled frequently.

**When to prefer `RingBuffer_PackedState`:** polling the buffer state frequently (e.g. waiting for incoming data in a loop), or when DMA is running in the background and bus bandwidth matters.
**When a split design would be faster:** very high `push()`/`pop()` throughput where saving 1 cycle and 1 bus transaction per call outweighs the cost of 1 extra bus transaction on state queries.

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
