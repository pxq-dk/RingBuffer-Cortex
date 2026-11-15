/*
 * RingBuffer_PackedState.h
 *
 *  Created on: 18. apr. 2026
 *      Author: Erik Nørskov
 *
 *  Copyright (c) 2026 Erik Nørskov / PxQ Technologies - https://pxq.dk
 *  SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-PxQ-Commercial
 *  License revision 1.1 - 2026-09-20
 *
 *  Dual-licensed: GPLv3 (strong copyleft - derivative works and products
 *  must also be GPLv3), or a commercial license from PxQ Technologies for
 *  proprietary use. A commercial license covers only the version actually
 *  delivered by the licensor; it permits defect fixes and documented
 *  configuration only, not new functionality, new use cases, or derivative
 *  works. Full terms: LICENSE (GPLv3) and COMMERCIAL-LICENSE.md, or
 *  https://pxq.dk
 */

#pragma once

inline constexpr const char* RINGBUFFER_PACKEDSTATE_VERSION = "1.5.0";

// Master switch for the runtime test code (Layer 1 selftest + Layer 2 stress
// test). Set to 0 to remove all test definitions, declarations, and the
// static stress buffer from the build — no flash or RAM cost. The constexpr
// mirror is provided for use in templated/generic code with `if constexpr`.
// Caller is responsible for guarding call sites with `#if RB_ENABLE_RUNTIME_TESTS`.
#define RB_ENABLE_RUNTIME_TESTS  0
inline constexpr bool RB_ENABLE_RUNTIME_TESTS_v = RB_ENABLE_RUNTIME_TESTS;

#if RB_ENABLE_RUNTIME_TESTS
// One-call entry point. Runs the Layer 1 compile-time-test suite at runtime
// (BKPT on fail), then arms the Layer 2 concurrent SPSC stress test
// (TIM6 producer ISR + main-loop consumer). Caller then needs to call
// RingBuffer_PackedState_stress_consumer_tick() inside the main loop.
// Precondition: MX_TIM6_Init() has run (RCC clock enabled, base init done).
void RingBuffer_PackedState_runtime_tests_init();

// Main-loop consumer hook. Waits until the test buffer is half full, then
// drains in a tight loop; BKPT on first sequence mismatch.
void RingBuffer_PackedState_stress_consumer_tick();

// Lower-level entry points exposed for users who want to drive the stress
// test from their own ISR / loop rather than the bundled runtime_tests_init.
bool RingBuffer_PackedState_runtime_selftest();
void RingBuffer_PackedState_stress_init();
void RingBuffer_PackedState_stress_producer_tick();

// Stats — volatile so the SWD watch window can inspect them live.
extern volatile uint32_t RingBuffer_PackedState_stress_pushes;
extern volatile uint32_t RingBuffer_PackedState_stress_drops;
extern volatile uint32_t RingBuffer_PackedState_stress_pops;
extern volatile uint32_t RingBuffer_PackedState_stress_seq_errors;
#endif

// Convenience macro: runs Layer 1 selftest + arms Layer 2 stress test + enters
// the consumer loop forever. `idle_expr` is expanded inline at the top of each
// iteration — typically a watchdog refresh. When RB_ENABLE_RUNTIME_TESTS=0
// the macro expands to a no-op, so the surrounding code can leave the call
// site unguarded and falls through to production logic.
#if RB_ENABLE_RUNTIME_TESTS
	#define RB_RUN_RUNTIME_TESTS(idle_expr)                          \
		do {                                                         \
			RingBuffer_PackedState_runtime_tests_init();             \
			for (;;) {                                               \
				(idle_expr);                                         \
				RingBuffer_PackedState_stress_consumer_tick();       \
			}                                                        \
		} while (0)
#else
	#define RB_RUN_RUNTIME_TESTS(idle_expr)  do { } while (0)
#endif

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <type_traits>
#include <atomic>      // std::atomic_signal_fence (non-GNU fallback only)

// Forced -Os on the library, independent of the project's optimisation level.
// Purpose: keep the buffer fast and small in -O0 Debug builds, where it often
// runs in ISRs. Measured 2026-11-07 (STM32G071, whole project at -O0):
// without these attributes single push()/pop() were 3-4x slower; with
// always_inline alone ~2x slower (the inlined code is then unoptimised).
// At -Os they are equal or better; at -O2 dropping them gains only 5-20 cycles.
// Side effect to remember: GCC does not inline a function with different
// optimisation settings unless it is always_inline. Every helper used inside
// an RB_OPT function must therefore be RB_OPT_INLINE (see ActiveGuard/NoGuard).
//
// RB_OPT_COPY: RB_OPT for push_n()/pop_n(), plus no-tree-loop-distribute-patterns
// so GCC keeps the small-block element loop instead of turning it into a memcpy
// call (see "Bulk copy" below).
#if defined(__GNUC__) || defined(__clang__)
	#define RB_OPT        [[gnu::optimize("Os")]]
	#define RB_OPT_INLINE [[gnu::optimize("Os"), gnu::always_inline]]
	#define RB_OPT_COPY   [[gnu::optimize("Os", "no-tree-loop-distribute-patterns")]]
	// Rarely taken paths (reservation slow paths): kept out of line so they do not
	// bloat every inlined push() call site or raise its register pressure.
	#define RB_OPT_COLD   [[gnu::optimize("Os", "no-tree-loop-distribute-patterns"), gnu::noinline, gnu::cold]]
#else
	#define RB_OPT
	#define RB_OPT_INLINE
	#define RB_OPT_COPY
	#define RB_OPT_COLD
#endif

// Bulk copy in push_n()/pop_n() (1.4.1): a plain element loop for short blocks,
// the C library's memcpy for longer ones. Where the switch pays off depends on
// the core, on the C library and on the element size, so the limit is chosen
// automatically:
//
//   C library / core                         element loop up to      then
//   newlib-nano (any core)                   always                  -
//   ARMv6-M (M0/M0+), any C library          always                  -
//   everything else (Thumb-2 cores, hosts)   8 elements, max 48 B    memcpy
//
// Measured from flash with GCC 14.3, cycles for push_n, mean over all head
// positions (nRF5340 Cortex-M33 64 MHz cache on; STM32G0B1 Cortex-M0+ 64 MHz):
// - newlib-nano's memcpy is a byte loop with call overhead: it never beats the
//   element loop (M33, 64 elements: uint8_t 381 vs 366, uint32_t 1346 vs 431).
//   There the code is exactly the plain loops of 1.4.0, memcpy is not referenced.
// - Thumb-2 with an optimised memcpy (full newlib): break-even at about 8
//   elements for uint8_t, uint16_t and uint32_t alike; at 64 elements the
//   hybrid is 2.9x / 2.6x / 2.0x faster than the loop (126 / 165 / 221 vs
//   366 / 431 / 431). A 12-byte struct breaks even at about 48 bytes.
//   Price: having both paths costs blocks of up to 8 elements 6-17 cycles
//   against the pure loop (one uint8_t: 57 vs 51; one uint32_t: 70 vs 53).
// - ARMv6-M has no unaligned access; full newlib's memcpy copies words only when
//   source AND destination are 4-byte aligned, and bytes otherwise. With the
//   head moving through the buffer it does not pay off for uint8_t / uint16_t
//   (64 elements: 586 vs 634 / 823 vs 712) or a 12-byte struct. For uint32_t it
//   wins only from about 24 elements on (64 elements: 570 vs 712) and costs
//   every shorter block up to 13 cycles - so ARMv6-M always uses the loop. Set
//   RB_COPY_LOOP_MAX (e.g. 16) if you move long blocks of 4-byte elements.
// newlib-nano is recognised by _NANO_FORMATTED_IO from its <newlib.h>, which
// needs --specs=nano.specs on the COMPILER command line too (STM32CubeIDE does
// that). If it is passed to the linker only, set RB_COPY_LOOP_MAX yourself.
// Non-trivially-copyable element types always use the element loop.
//
// RB_COPY_LOOP_MAX - optional override, define before the include: blocks of up
//   to this many ELEMENTS use the element loop, longer ones memcpy - for every
//   element type. 0 = always memcpy; a value >= the buffer size = never memcpy.
//   A macro so it can be set from the build system (-DRB_COPY_LOOP_MAX=16). Use
//   the SAME value (or none) in every translation unit of a program, or
//   different TUs get different definitions of the same inline functions (ODR
//   violation).
inline constexpr size_t RINGBUFFER_PACKEDSTATE_COPY_NEVER_MEMCPY = static_cast<size_t>(-1);
#ifdef RB_COPY_LOOP_MAX
static_assert((RB_COPY_LOOP_MAX) >= 0, "RB_COPY_LOOP_MAX must be a non-negative number of elements");
#endif
// Largest block (in elements) copied with the element loop, for an element of
// the given size.
constexpr size_t RingBuffer_PackedState_copy_loop_max([[maybe_unused]] size_t elem_size) {
#if defined(RB_COPY_LOOP_MAX)
	return static_cast<size_t>(RB_COPY_LOOP_MAX);
#elif defined(_NANO_FORMATTED_IO) || (defined(__ARM_ARCH_ISA_THUMB) && (__ARM_ARCH_ISA_THUMB == 1))
	return RINGBUFFER_PACKEDSTATE_COPY_NEVER_MEMCPY;
#else
	return (elem_size * 8 > 48) ? 48 / elem_size : 8;
#endif
}

// States a fact the compiler cannot derive; emits no code.
#if defined(__GNUC__) || defined(__clang__)
	#define RB_ASSUME(cond) do { if (!(cond)) __builtin_unreachable(); } while (0)
#else
	#define RB_ASSUME(cond) do { } while (0)
#endif

// The builtin folds to a constant even at -O0. std::is_constant_evaluated() is
// a libstdc++ wrapper that, in -O0 Debug builds, is emitted as a real
// `bl std::is_constant_evaluated` + compare + branch at every inlined use.
#if defined(__GNUC__) || defined(__clang__)
	#define RB_IS_CONSTANT_EVALUATED() __builtin_is_constant_evaluated()
#else
	#define RB_IS_CONSTANT_EVALUATED() std::is_constant_evaluated()
#endif


// Default LockImpl: masks all configurable interrupts via PRIMASK, nestable
// (unlock restores the previous state instead of blindly re-enabling).
//
// Self-contained: no CMSIS declarations needed before this header. 1.3.2 called
// __get_PRIMASK/__disable_irq/__set_PRIMASK; PrimaskLock is a non-template
// class, so those non-dependent names were looked up when the header was
// parsed — every user had to include CMSIS first, even for SPSC/None, which
// never lock. The asm below is identical to CMSIS cmsis_gcc.h, "memory"
// clobbers included: a lock must also be a compiler barrier, or GCC could
// move accesses out of the critical section.
//
// Off Cortex-M (e.g. host unit tests) the functions are templates, so their
// bodies are only checked when used: SPSC/None compile fine, and actually
// locking with PrimaskLock gives a clear error instead of a missing name.
#if (defined(__GNUC__) || defined(__clang__)) && defined(__ARM_ARCH_PROFILE) && (__ARM_ARCH_PROFILE == 'M')
struct PrimaskLock {
	static inline __attribute__((always_inline)) uint32_t lock() {
		uint32_t p;
		__asm__ volatile("mrs %0, primask" : "=r"(p) :: "memory");
		__asm__ volatile("cpsid i" ::: "memory");
		return p;
	}
	static inline __attribute__((always_inline)) void unlock(uint32_t p) {
		__asm__ volatile("msr primask, %0" :: "r"(p) : "memory");
	}
};
#else
struct PrimaskLock {
	template<int N = 0> static uint32_t lock() {
		static_assert(N != 0, "PrimaskLock needs a Cortex-M target (GCC/Clang); pass your own LockImpl, e.g. Topology::MPSC<MyLock>");
		return 0;
	}
	template<int N = 0> static void unlock(uint32_t) {
		static_assert(N != 0, "PrimaskLock needs a Cortex-M target (GCC/Clang); pass your own LockImpl, e.g. Topology::MPSC<MyLock>");
	}
};
#endif

struct Topology {
	// Single context — no concurrency at all.
	template<typename LockImpl = PrimaskLock>
	struct None : LockImpl {
		static constexpr bool needs_volatile = false;
		static constexpr bool lock_p_needed  = false;
		static constexpr bool lock_c_needed  = false;
	};

	// Single Producer, Single Consumer — e.g. ISR owns head and main owns tail, or vice versa.
	// No guard on either side; volatile ensures actual LDRH/STRH on every access.
	template<typename LockImpl = PrimaskLock>
	struct SPSC : LockImpl {
		static constexpr bool needs_volatile = true;
		static constexpr bool lock_p_needed  = false;
		static constexpr bool lock_c_needed  = false;
	};

	// Multiple Producers, Single Consumer — producer side guarded, consumer side free.
	// MaxReservations (0, 1, 2, 4 or 8): size of the producer reservation table -
	// how many reservations may be open at the same time (reserve() / commit(),
	// see RingBuffer_PackedState for the exact rule). 0 = the feature is compiled
	// out completely - no table, no extra compare, no code.
	template<typename LockImpl = PrimaskLock, size_t MaxReservations = 0>
	struct MPSC : LockImpl {
		static constexpr bool   needs_volatile   = true;
		static constexpr bool   lock_p_needed    = true;
		static constexpr bool   lock_c_needed    = false;
		static constexpr size_t max_reservations = MaxReservations;
	};

	// Single Producer, Multiple Consumers — consumer side guarded, producer side free.
	template<typename LockImpl = PrimaskLock>
	struct SPMC : LockImpl {
		static constexpr bool needs_volatile = true;
		static constexpr bool lock_p_needed  = false;
		static constexpr bool lock_c_needed  = true;
	};

	// Internal marker — NOT a sixth topology; never write it yourself.
	// It is the default when no topology is given, and behaves exactly like
	// None. It exists only so the library can tell "omitted" apart from an
	// explicit None<> and emit a deprecation warning (1.4.0). Removed in 2.0,
	// when the topology argument becomes mandatory.
	template<typename LockImpl = PrimaskLock>
	struct Unspecified_ : None<LockImpl> {};

	// Multiple Producers, Multiple Consumers — both sides guarded.
	// MaxReservations: as for MPSC (producer-side reservations).
	template<typename LockImpl = PrimaskLock, size_t MaxReservations = 0>
	struct MPMC : LockImpl {
		static constexpr bool   needs_volatile   = true;
		static constexpr bool   lock_p_needed    = true;
		static constexpr bool   lock_c_needed    = true;
		static constexpr size_t max_reservations = MaxReservations;
	};
};

// Lock for the compile-time unit tests of locking topologies: usable in
// constant expressions, does nothing. Never use it in real code.
struct RingBuffer_PackedState_ConstexprTestLock {
	static constexpr uint32_t lock() { return 0; }
	static constexpr void     unlock(uint32_t) {}
};
// Policy for the compile-time reservation tests: MPSC flags with a reservation
// table, but needs_volatile = false - reading volatile objects is not allowed
// in constant expressions. The tests are single-threaded, so volatile does not
// matter for the logic under test. Never use it in real code.
struct RingBuffer_PackedState_ConstexprReservationTestPolicy : RingBuffer_PackedState_ConstexprTestLock {
	static constexpr bool   needs_volatile   = false;
	static constexpr bool   lock_p_needed    = true;
	static constexpr bool   lock_c_needed    = false;
	static constexpr size_t max_reservations = 4;
};

template<typename RBType>
class unit_test_ringbuffer
{
	public:
	using value_type = typename RBType::value_type;
	static constexpr size_t capacity = RBType::capacity;

	static constexpr bool run_test()
	{
		// New buffer must be empty and not full
		{ RBType rb; if (!rb.isEmpty())  return false; }
		{ RBType rb; if ( rb.isFull())   return false; }

		// Not empty after push
		{ RBType rb; rb.push(1); if (rb.isEmpty()) return false; }

		// Full after capacity pushes
		{ RBType rb; for (size_t i = 0; i < capacity; ++i) rb.push(static_cast<value_type>(i + 1)); if (!rb.isFull()) return false; }

		// Push to full must return false
		{ RBType rb; for (size_t i = 0; i < capacity; ++i) rb.push(static_cast<value_type>(i + 1)); if (rb.push(99)) return false; }

		// Pop from empty must return false
		{ RBType rb; value_type v{}; if (rb.pop(v)) return false; }

		// Popped value must match pushed value
		{ RBType rb; rb.push(static_cast<value_type>(42)); value_type v{}; rb.pop(v); if (v != 42) return false; }

		// Must maintain FIFO order
		{ RBType rb; rb.push(static_cast<value_type>(1)); rb.push(static_cast<value_type>(2)); rb.push(static_cast<value_type>(3));
		  value_type a{}, b{}, c{}; rb.pop(a); rb.pop(b); rb.pop(c);
		  if (a != 1 || b != 2 || c != 3) return false; }

		// getCount must return 0 when empty
		{ RBType rb; if (rb.getCount() != 0) return false; }

		// getCount must increment with each push
		{ RBType rb; for (size_t i = 0; i < capacity; ++i) { rb.push(static_cast<value_type>(i + 1)); if (rb.getCount() != i + 1) return false; } }

		// getCount must equal capacity when full
		{ RBType rb; for (size_t i = 0; i < capacity; ++i) rb.push(static_cast<value_type>(i + 1)); if (rb.getCount() != capacity) return false; }

		// getCount must decrement with each pop
		{ RBType rb; for (size_t i = 0; i < capacity; ++i) rb.push(static_cast<value_type>(i + 1));
		  for (size_t i = 0; i < capacity; ++i) { value_type v{}; rb.pop(v); if (rb.getCount() != capacity - i - 1) return false; } }

		// Contiguous push area: write via pointer, commit, read back with pop
		{ RBType rb;
		  auto area = rb.get_contiguous_push_area(capacity);
		  for (size_t i = 0; i < area.count; ++i) area.ptr[i] = static_cast<value_type>(i + 1);
		  rb.commit_push(area.count);
		  if (rb.getCount() != area.count) return false;
		  for (size_t i = 0; i < area.count; ++i) { value_type v{}; rb.pop(v); if (v != static_cast<value_type>(i + 1)) return false; } }

		// Contiguous pop area: push with push(), read back via pointer and commit
		{ RBType rb;
		  for (size_t i = 0; i < capacity; ++i) rb.push(static_cast<value_type>(i + 1));
		  auto area = rb.get_contiguous_pop_area(capacity);
		  for (size_t i = 0; i < area.count; ++i) { if (area.ptr[i] != static_cast<value_type>(i + 1)) return false; }
		  rb.commit_pop(area.count);
		  if (rb.getCount() != 0) return false; }

		// Reserve push: advance head first, write after, verify with pop
		{ RBType rb;
		  auto area = rb.reserve_push(capacity);
		  for (size_t i = 0; i < area.count; ++i) area.ptr[i] = static_cast<value_type>(i + 5);
		  for (size_t i = 0; i < area.count; ++i) { value_type v{}; rb.pop(v); if (v != static_cast<value_type>(i + 5)) return false; } }

		// Reserve pop: advance tail first, read after, verify count is zero
		{ RBType rb;
		  for (size_t i = 0; i < capacity; ++i) rb.push(static_cast<value_type>(i + 1));
		  auto area = rb.reserve_pop(capacity);
		  for (size_t i = 0; i < area.count; ++i) { if (area.ptr[i] != static_cast<value_type>(i + 1)) return false; }
		  if (rb.getCount() != 0) return false; }

		// max_size clamping: request more than available, verify count is clamped
		{ RBType rb;
		  rb.push(static_cast<value_type>(1));
		  auto push_area = rb.get_contiguous_push_area(capacity + 10);
		  if (push_area.count > capacity - 1) return false;
		  auto pop_area  = rb.get_contiguous_pop_area(capacity + 10);
		  if (pop_area.count > 1) return false; }

		// Empty buffer returns count=0 from get_contiguous_pop_area
		{ RBType rb;
		  auto area = rb.get_contiguous_pop_area(capacity);
		  if (area.count != 0) return false; }

		// Wrap-around: contiguous count splits at array boundary
		{ RBType rb;
		  // Advance head to near end of array by pushing and popping
		  for (size_t i = 0; i < capacity - 1; ++i) rb.push(static_cast<value_type>(i + 1));
		  for (size_t i = 0; i < capacity - 1; ++i) { value_type v{}; rb.pop(v); }
		  // Head is now at index (capacity-1), one slot from array end (Size=4, head=2)
		  // Push one element past the boundary
		  rb.push(static_cast<value_type>(7));
		  rb.push(static_cast<value_type>(8));
		  // get_contiguous_pop_area must not report more than what is contiguous before wrap
		  auto area = rb.get_contiguous_pop_area(capacity);
		  if (area.count == 0) return false;
		  if (area.ptr[0] != static_cast<value_type>(7)) return false; }

		// Split pop area: tail at last array index, data wraps — requires two get_contiguous_pop_area calls
		{ RBType rb;
		  // Fill and drain completely to advance tail to last array index (Size-1)
		  for (size_t i = 0; i < capacity; ++i) rb.push(static_cast<value_type>(i + 1));
		  for (size_t i = 0; i < capacity; ++i) { value_type v{}; rb.pop(v); }
		  // Push 2 elements: first lands at index Size-1, second wraps to index 0
		  rb.push(static_cast<value_type>(7));
		  rb.push(static_cast<value_type>(8));
		  // First area: only one element contiguous before wrap
		  auto area1 = rb.get_contiguous_pop_area(capacity);
		  if (area1.count != 1) return false;
		  if (area1.ptr[0] != static_cast<value_type>(7)) return false;
		  rb.commit_pop(area1.count);
		  // Second area: one element after wrap
		  auto area2 = rb.get_contiguous_pop_area(capacity);
		  if (area2.count != 1) return false;
		  if (area2.ptr[0] != static_cast<value_type>(8)) return false;
		  rb.commit_pop(area2.count);
		  if (rb.getCount() != 0) return false; }

		// clear(): empties a partly filled, wrapped buffer; full capacity and FIFO work afterwards
		{ RBType rb;
		  for (size_t i = 0; i < capacity; ++i) rb.push(static_cast<value_type>(i + 1));
		  { value_type v{}; rb.pop(v); }
		  rb.push(static_cast<value_type>(9));                       // head has wrapped
		  rb.clear();
		  if (!rb.isEmpty() || rb.getCount() != 0) return false;
		  { value_type v{}; if (rb.pop(v)) return false; }
		  for (size_t i = 0; i < capacity; ++i) if (!rb.push(static_cast<value_type>(i + 20))) return false;
		  if (!rb.isFull()) return false;
		  for (size_t i = 0; i < capacity; ++i) { value_type v{}; rb.pop(v); if (v != static_cast<value_type>(i + 20)) return false; } }

		// push_n/pop_n: clamp to free space / available count, wrap, FIFO order
		{ RBType rb;
		  value_type src[capacity + 2] {}; value_type dst[capacity + 2] {};
		  for (size_t i = 0; i < capacity + 2; ++i) src[i] = static_cast<value_type>(i + 30);
		  if (rb.push_n(src, capacity + 2) != capacity) return false;        // clamped to free space
		  if (!rb.isFull()) return false;
		  if (rb.pop_n(dst, 2) != 2) return false;
		  if (dst[0] != static_cast<value_type>(30) || dst[1] != static_cast<value_type>(31)) return false;
		  if (rb.push_n(src, 2) != 2) return false;                          // head wraps
		  if (rb.pop_n(dst, capacity + 2) != capacity) return false;         // clamped to count, tail wraps
		  for (size_t i = 0; i < capacity - 2; ++i) if (dst[i] != static_cast<value_type>(i + 32)) return false;
		  if (dst[capacity - 2] != static_cast<value_type>(30) || dst[capacity - 1] != static_cast<value_type>(31)) return false;
		  if (!rb.isEmpty() || rb.pop_n(dst, 1) != 0) return false; }

		// FIFO order must be maintained across a wrap-around
		{ RBType rb;
		  for (size_t i = 0; i < capacity; ++i) rb.push(static_cast<value_type>(i + 1));
		  for (size_t i = 0; i < capacity / 2; ++i) { value_type v{}; rb.pop(v); }
		  for (size_t i = 0; i < capacity / 2; ++i) rb.push(static_cast<value_type>(i + 10));
		  for (size_t i = capacity / 2; i < capacity; ++i) { value_type v{}; rb.pop(v); if (v != static_cast<value_type>(i + 1)) return false; }
		  for (size_t i = 0; i < capacity / 2; ++i) { value_type v{}; rb.pop(v); if (v != static_cast<value_type>(i + 10)) return false; } }

		return true;
	}
};

// Compile-time tests of producer reservations (reserve()/commit()). RBType must
// have max_reservations >= 2 and Size >= 8; run with a constexpr test lock.
template<typename RBType>
class unit_test_reservations
{
	public:
	using value_type = typename RBType::value_type;
	static constexpr size_t capacity = RBType::capacity;
	static constexpr bool run_test()
	{
		value_type v{};
		// Out-of-order finish: B done first stays hidden until the older A is done.
		{ RBType rb;
		  auto a = rb.reserve(2); auto b = rb.reserve(2);
		  if (a.count != 2 || b.count != 2) return false;
		  b.ptr[0] = static_cast<value_type>(3); b.ptr[1] = static_cast<value_type>(4); rb.commit(b);
		  if (rb.getCount() != 0) return false;
		  a.ptr[0] = static_cast<value_type>(1); a.ptr[1] = static_cast<value_type>(2); rb.commit(a);
		  if (rb.getCount() != 4) return false;
		  for (int i = 1; i <= 4; ++i) { if (!rb.pop(v) || v != static_cast<value_type>(i)) return false; } }

		// Continuous overlap: the next reservation is always opened before the
		// previous one commits - release must still progress (no starvation).
		{ RBType rb; size_t got = 0;
		  auto prev = rb.reserve(1); prev.ptr[0] = static_cast<value_type>(1);
		  for (int round = 0; round < 3 * static_cast<int>(capacity); ++round) {
			  auto next = rb.reserve(1); if (next.count != 1) return false;
			  next.ptr[0] = static_cast<value_type>(1);
			  rb.commit(prev);
			  if (rb.getCount() == 0) return false;
			  while (rb.pop(v)) ++got;
			  prev = next; }
		  rb.commit(prev); while (rb.pop(v)) ++got;
		  if (got != 3 * capacity + 1) return false; }

		// push() and push_n() behind an open reservation must not publish it.
		{ RBType rb;
		  auto a = rb.reserve(1);
		  value_type two[2] = { static_cast<value_type>(8), static_cast<value_type>(9) };
		  if (!rb.push(static_cast<value_type>(7)) || rb.push_n(two, 2) != 2) return false;
		  if (rb.getCount() != 0) return false;
		  a.ptr[0] = static_cast<value_type>(6); rb.commit(a);
		  if (rb.getCount() != 4) return false;
		  for (int i = 6; i <= 9; ++i) { if (!rb.pop(v) || v != static_cast<value_type>(i)) return false; } }

		// Table full: one more reservation is refused, but push and push_n still work
		// (they take no table entry) and stay hidden behind the open reservations.
		{ RBType rb;
		  typename RBType::Reservation r[RBType::max_reservations] {};
		  for (size_t i = 0; i < RBType::max_reservations; ++i) { r[i] = rb.reserve(1); if (r[i].count != 1) return false; r[i].ptr[0] = static_cast<value_type>(i + 1); }
		  value_type one[1] = { static_cast<value_type>(99) };
		  if (rb.reserve(1).count != 0) return false;
		  if (!rb.push(static_cast<value_type>(98)) || rb.push_n(one, 1) != 1) return false;
		  if (rb.getCount() != 0) return false;
		  for (size_t i = RBType::max_reservations; i-- > 0; ) rb.commit(r[i]);   // reverse order
		  if (rb.getCount() != RBType::max_reservations + 2) return false;
		  for (size_t i = 0; i < RBType::max_reservations; ++i) { if (!rb.pop(v) || v != static_cast<value_type>(i + 1)) return false; }
		  if (!rb.pop(v) || v != static_cast<value_type>(98) || !rb.pop(v) || v != static_cast<value_type>(99)) return false; }

		// reserve()+commit() behind an open reservation takes no table entry for
		// good: far more of them than the table has entries, all stay hidden.
		{ RBType rb; size_t got = 0;
		  auto a = rb.reserve(1); a.ptr[0] = static_cast<value_type>(1);
		  for (size_t i = 0; i < 2 * RBType::max_reservations + 1 && i + 2 < capacity; ++i) {
			  auto b = rb.reserve(1); if (b.count != 1) return false;
			  b.ptr[0] = static_cast<value_type>(i + 2); rb.commit(b); ++got;
			  if (rb.getCount() != 0) return false; }
		  rb.commit(a);
		  if (rb.getCount() != got + 1) return false;
		  for (size_t i = 0; i < got + 1; ++i) { if (!rb.pop(v) || v != static_cast<value_type>(i + 1)) return false; } }

		// commit(r, used): shrinks while nothing is behind r, gives everything back
		// with used = 0, and publishes the whole block once something is behind it.
		{ RBType rb;
		  auto a = rb.reserve(4); if (a.count != 4) return false;
		  a.ptr[0] = static_cast<value_type>(1); a.ptr[1] = static_cast<value_type>(2);
		  if (rb.commit(a, 2) != 2 || rb.getCount() != 2) return false;            // shrunk 4 -> 2
		  auto b = rb.reserve(3); if (b.count != 3) return false;
		  if (rb.commit(b, 0) != 0 || rb.getCount() != 2) return false;            // given back
		  auto c = rb.reserve(2); auto d = rb.reserve(1);
		  if (c.count != 2 || d.count != 1) return false;
		  c.ptr[0] = static_cast<value_type>(3); c.ptr[1] = static_cast<value_type>(0);   // [1] = padding
		  if (rb.commit(c, 1) != 2) return false;                                  // d is behind: cannot shrink
		  if (rb.commit(d, 0) != 0) return false;                                  // youngest: given back
		  if (!rb.push(static_cast<value_type>(4))) return false;
		  if (rb.getCount() != 5) return false;
		  const int expect[5] = { 1, 2, 3, 0, 4 };
		  for (int i = 0; i < 5; ++i) { if (!rb.pop(v) || v != static_cast<value_type>(expect[i])) return false; }
		  if (!rb.isEmpty()) return false; }

		// Wrap: a reservation stops at the array end, the next one continues at 0.
		{ RBType rb;
		  for (size_t i = 0; i + 2 < capacity; ++i) { rb.push(static_cast<value_type>(0)); rb.pop(v); }  // index -> Size-3
		  auto a = rb.reserve(8); auto b = rb.reserve(2);
		  if (a.count != 3 || b.count != 2) return false;
		  for (size_t i = 0; i < 3; ++i) a.ptr[i] = static_cast<value_type>(i + 1);
		  b.ptr[0] = static_cast<value_type>(4); b.ptr[1] = static_cast<value_type>(5);
		  rb.commit(b); rb.commit(a);
		  for (int i = 1; i <= 5; ++i) { if (!rb.pop(v) || v != static_cast<value_type>(i)) return false; }
		  if (!rb.isEmpty()) return false; }

		// A refused (count == 0) reservation may be committed harmlessly.
		{ RBType rb; typename RBType::Reservation none {}; rb.commit(none); if (!rb.isEmpty()) return false; }
		return true;
	}
};

// Emitted (as a deprecation warning, pointing at the user's line) when a
// RingBuffer_PackedState is declared without a topology. A template so that
// it is only checked when actually used: a non-template call would be looked
// up when the header is parsed and warn every user (two-phase lookup).
template<typename Policy>
[[deprecated("RingBuffer_PackedState: no topology given - defaulting to Topology::None<> "
             "(single context, NOT interrupt-safe). Pass it explicitly: Topology::None<>, "
             "SPSC<> (e.g. ISR -> main), MPSC<>, SPMC<> or MPMC<>. "
             "The default is removed in 2.0.")]]
constexpr void RingBuffer_PackedState_topology_not_specified() {}

// Optimised ring buffer with head and tail as adjacent volatile uint16_t fields.
// Reads are LDRH, writes are STRH — in SPSC each side owns one field exclusively.
// One slot is sacrificed to distinguish full from empty without a count variable.
// Effective capacity is Size-1.
// IrqPolicy: always pass it explicitly (see Topology). Omitting it still means
// None (unchanged behaviour) but is deprecated since 1.4.0 and fails in 2.0.
template<typename T, size_t Size, typename IrqPolicy = Topology::Unspecified_<>, bool _IsTestInstance = false>
class RingBuffer_PackedState
{
	static_assert(Size >= 2 && Size <= 65535, "RingBuffer_PackedState: Size must be in range [2, 65535]");

	private:
	// Two separate guard types instead of one with `if constexpr` inside:
	// the old single GuardImpl<false> still had a user-provided destructor, and
	// as a non-RB_OPT_INLINE function it was not inlined into RB_OPT functions
	// in -O0 builds - push_n() made a real call to an EMPTY destructor and had to
	// save 8 instead of 5 registers. NoGuard is trivially destructible, so there
	// is nothing to call; ActiveGuard's lock/unlock are forced inline.
	struct ActiveGuard {
		uint32_t primask;
		RB_OPT_INLINE constexpr ActiveGuard()  : primask(IrqPolicy::lock()) {}
		RB_OPT_INLINE constexpr ~ActiveGuard() { IrqPolicy::unlock(primask); }
	};
	struct NoGuard {
		RB_OPT_INLINE constexpr NoGuard() noexcept {}
	};
	template<bool Active>
	using GuardImpl = std::conditional_t<Active, ActiveGuard, NoGuard>;
	using ProducerGuard = GuardImpl<IrqPolicy::lock_p_needed>;
	using ConsumerGuard = GuardImpl<IrqPolicy::lock_c_needed>;
	// clear() writes both fields, so it must exclude every side that has a lock.
	using ClearGuard    = GuardImpl<IrqPolicy::lock_p_needed || IrqPolicy::lock_c_needed>;

	static constexpr bool is_power_of_two = (Size & (Size - 1)) == 0;

	using half_type    = std::conditional_t<IrqPolicy::needs_volatile, volatile uint16_t, uint16_t>;

	// Elements are deliberately NOT volatile (since 1.3.3). Each slot is owned by
	// exactly one side at a time (producer between snapshot and head publish,
	// consumer between snapshot and tail publish), so no access to a slot ever
	// races. What must be guaranteed is ORDER relative to the volatile index
	// accesses — and volatile on the elements could not give that to a bulk copy
	// or to user code writing through a zero-copy pointer with volatile cast away.
	// The order is enforced by acquire_barrier() / release_barrier() below.
	using element_type = T;

	// ---- producer reservations (opt-in: MPSC/MPMC<Lock, N>, N > 0) ----------
	//
	// Several producers may each hold an open reservation and fill it OUTSIDE the
	// lock (copy, DMA). state.head then means "committed": everything up to the
	// start of the OLDEST unfinished reservation. A reservation that finishes
	// early is marked done and released together with the older one(s) when
	// those finish - in order, without anybody ever waiting:
	//
	//    tail        state.head (committed)        res.reserved
	//     |               |                             |
	//  [ consumer data ][ open / done reservations ][ free ]
	//
	// res.reserved is only maintained while a reservation is open (res.count != 0);
	// with none open the next free slot is state.head. The plain push()/push_n()
	// fast path therefore is today's code plus one compare, and writes nothing to res.
	// All res fields are producer-private and only touched under ProducerGuard.
	template<typename P>
	static constexpr size_t max_reservations_of() {
		if constexpr (requires { P::max_reservations; }) return P::max_reservations;
		else return 0;
	}
	public:
	static constexpr size_t max_reservations = max_reservations_of<IrqPolicy>();
	private:
	static_assert(max_reservations <= 8 && (max_reservations & (max_reservations - 1)) == 0,
	              "RingBuffer_PackedState: MaxReservations must be 0, 1, 2, 4 or 8");

	template<size_t N, typename = void>
	struct ResState {
		uint16_t reserved = 0;       // next slot to hand out
		uint8_t  first    = 0;       // table index of the oldest open reservation
		uint8_t  count    = 0;       // open reservations
		uint8_t  done     = 0;       // bit i: entry i finished, waiting for older ones
		uint16_t end[N]   {};        // end index (exclusive) per open reservation
	};
	template<typename D>
	struct ResState<0, D> {};        // feature off: empty, occupies no storage

	// Adjacent halfwords — head written by producer (STRH), tail written by consumer (STRH).
	alignas(4) struct { half_type head; half_type tail; } state;
	// Right after state: small offsets, so the fast path reaches res.count with a
	// single LDRB (Cortex-M0+ immediate offsets reach only 31 bytes). Reservations
	// off: an empty type that, with [[no_unique_address]], occupies no storage -
	// buffer keeps its offset and the object its size.
	[[no_unique_address]] ResState<max_reservations> res;
	alignas(alignof(T)) element_type buffer[Size];

	// Compiler barriers ordering element accesses against the index accesses.
	// On single-core Cortex-M an ISR observes memory in program order, so a
	// compiler-only barrier (no DMB) is sufficient for ISR <-> thread sharing.
	// Zero instructions are emitted; they only stop GCC from moving memory
	// accesses across them. Compiled out for Topology::None.
	//
	// acquire_barrier(): after readHT() — element accesses must not be hoisted
	//   above the snapshot that proved the slots belong to this side.
	// release_barrier(): before publishing head/tail — element accesses must be
	//   complete before the other side can see the slots handed over.
	//
	// A full "memory" clobber was measured smaller than an asm scoped to
	// "+m"(buffer), "+m"(state) — both at -O2 and in the -O0 Debug build.
	RB_OPT_INLINE static constexpr void compiler_barrier() {
		if constexpr (IrqPolicy::needs_volatile) {
			if (!RB_IS_CONSTANT_EVALUATED()) {
#if defined(__GNUC__) || defined(__clang__)
				__asm__ volatile("" ::: "memory");
#else
				std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
			}
		}
	}
	RB_OPT_INLINE static constexpr void acquire_barrier() { compiler_barrier(); }
	RB_OPT_INLINE static constexpr void release_barrier() { compiler_barrier(); }

	// Non-volatile snapshot — same layout as state, always uint16_t regardless of IrqPolicy.
	struct StateSnapshot { uint16_t head; uint16_t tail; };

	// Atomic snapshot of head and tail. At runtime: single 32-bit LDR into StateSnapshot
	// (state is alignas(4), same layout). Volatile only when the policy requires it
	// (ISR concurrency possible) — otherwise the compiler is free to CSE repeated reads.
	// At compile time: two separate reads (reinterpret_cast not allowed in constexpr).
	// The whole state word in one 32-bit LDR: head in bits 0..15, tail in 16..31
	// (little-endian, state is alignas(4)).
	RB_OPT_INLINE constexpr uint32_t readRaw() const {
		if (RB_IS_CONSTANT_EVALUATED()) {
			return static_cast<uint32_t>(static_cast<uint16_t>(state.head))
			     | (static_cast<uint32_t>(static_cast<uint16_t>(state.tail)) << 16);
		}
		typedef uint32_t __attribute__((may_alias)) u32_alias;
		if constexpr (IrqPolicy::needs_volatile) {
			return *reinterpret_cast<const volatile u32_alias*>(&state);
		} else {
			return *reinterpret_cast<const u32_alias*>(&state);
		}
	}

	RB_OPT_INLINE constexpr StateSnapshot readHT() const {
		uint32_t raw = readRaw();
		return { static_cast<uint16_t>(raw), static_cast<uint16_t>(raw >> 16) };
	}

	// Empty test on the raw word, without extracting head and tail first.
	// Extracting both (uxth + lsr) and keeping copies alive cost pop() a stack
	// frame ({r4, r5, lr} on M33 at -Os); these forms need no extra registers.
	//   Thumb-2 (M3/M4/M7/M33): head == tail  <=>  word == rotr(word, 16)
	//                            -> a single `cmp rX, rX, ror #16`.
	//   ARMv6-M (M0/M0+):        no shifted operands; ((w ^ (w >> 16)) & 0xFFFF) == 0
	//                            measured 2 instructions shorter there.
	// Both are exact for any word (tail and head are each 16 bits).
	RB_OPT_INLINE static constexpr bool rawEmpty(uint32_t w) {
#if defined(__ARM_ARCH_ISA_THUMB) && (__ARM_ARCH_ISA_THUMB >= 2)
		return w == ((w >> 16) | (w << 16));
#else
		return ((w ^ (w >> 16)) & 0xFFFFu) == 0;
#endif
	}

	RB_OPT_INLINE constexpr uint16_t nextIndex(uint16_t idx) const {
		if constexpr (is_power_of_two) return (idx + 1) & static_cast<uint16_t>(Size - 1);
		else                           return (idx + 1 >= static_cast<uint16_t>(Size)) ? static_cast<uint16_t>(0) : static_cast<uint16_t>(idx + 1);
	}

	RB_OPT_INLINE constexpr size_t wrapSize(uint32_t val) const {
		if constexpr (is_power_of_two) return val & (Size - 1);
		else                           return (val >= Size) ? val - Size : val;
	}

	// Bulk copy for push_n()/pop_n(): element loops for blocks of up to
	// copy_loop_max elements, memcpy above (see RB_COPY_LOOP_MAX for how the limit
	// is chosen). The block is tested ONCE as a whole (copy_with_memcpy), not once
	// per segment, and the two segments are then copied one after the other - so
	// the short-block path is the two plain loops of 1.4.0 plus a single compare.
	// Constant evaluation and non-trivially-copyable T always use the loops; where
	// memcpy is never used the test folds to false and memcpy is not referenced.
	static constexpr size_t copy_loop_max = RingBuffer_PackedState_copy_loop_max(sizeof(T));
	RB_OPT_INLINE static constexpr bool copy_with_memcpy(size_t count) {
		if constexpr (std::is_trivially_copyable_v<T> && copy_loop_max != RINGBUFFER_PACKEDSTATE_COPY_NEVER_MEMCPY) {
			// copy_loop_max == 0 (large elements, or RB_COPY_LOOP_MAX = 0): always
			// memcpy, also for count == 0 (copy_memcpy skips n == 0). The loop path is
			// then dead code at run time; left reachable only for count == 0, GCC's
			// loop analysis warns about it (-Waggressive-loop-optimizations).
			if (!RB_IS_CONSTANT_EVALUATED()) return copy_loop_max == 0 || count > copy_loop_max;
		}
		return false;
	}
	RB_OPT_INLINE static constexpr void copy_loop(T* d, const T* s, size_t n) {
		// Counts down in a bottom-tested loop: at -Os GCC turns the usual
		// `for (i = 0; i < n; ++i)` into a loop with TWO taken branches per element
		// (test at the top, jump back). This form has one - measured 8 instead of
		// 10 cycles per byte on Cortex-M0+, 6 instead of 7 on Cortex-M33. The
		// order within a block does not matter: it is published afterwards.
		if (n != 0) { do { --n; d[n] = s[n]; } while (n != 0); }
	}
	// Only called when copy_with_memcpy() is true. n may be 0 (no wrapped part).
	RB_OPT_INLINE static void copy_memcpy(T* d, const T* s, size_t n) {
		// Callers clamp n to the buffer size; stating it lets GCC prove the length
		// bound - otherwise -Wstringop-overflow fires on `count - first`.
		// if constexpr: this function is instantiated for every T that uses
		// push_n()/pop_n(), also where it is never called - and memcpy on a
		// non-trivially-copyable T would warn (-Wclass-memaccess) even as dead code.
		if constexpr (std::is_trivially_copyable_v<T>) {
			RB_ASSUME(n <= Size);
			if (n != 0) memcpy(d, s, n * sizeof(T));
		}
	}

	// Under ProducerGuard. n must fit (caller checked free space and table).
	RB_OPT_INLINE constexpr uint8_t res_alloc(uint16_t r, size_t n) {
		uint8_t slot = static_cast<uint8_t>((res.first + res.count) & (max_reservations - 1));
		res.reserved  = static_cast<uint16_t>(wrapSize(r + static_cast<uint32_t>(n)));
		res.end[slot] = res.reserved;
		res.done      = static_cast<uint8_t>(res.done & ~(1u << slot));
		++res.count;
		return slot;
	}
	// Under ProducerGuard. Marks `slot` done and tidies the table from both ends:
	//  1. a finished entry at the YOUNG end is merged into its older neighbour
	//     (which then ends where the merged one ended) and leaves the table - so
	//     reserve()+commit() behind a still open reservation costs no table entry;
	//  2. finished entries at the OLD end are released: the committed head is
	//     published up to the end of the last one.
	// A finished entry BETWEEN two open ones keeps its place until one of them
	// finishes (the younger one's slot number cannot change). At most N steps each.
	RB_OPT_INLINE constexpr void res_finish(uint8_t slot) {
		release_barrier();                                   // every fill complete before anything is marked
		uint8_t cnt = res.count;
		if (cnt == 1) {                                      // the only entry (the usual case): publish it,
			res.count  = 0;                                  // the table is empty again
			state.head = res.end[slot];                      // single STRH
			return;
		}
		// Several entries. At rest neither the oldest nor the youngest entry is
		// marked done (a finished oldest is released, a finished youngest merged),
		// so exactly one of three things happens:
		const uint8_t mask  = static_cast<uint8_t>(max_reservations - 1);
		uint8_t       first = res.first;
		uint8_t       done  = res.done;
		uint8_t       y     = static_cast<uint8_t>((first + cnt - 1) & mask);
		if (slot == y) {
			// 1. the youngest finished: merge it into its older neighbour - and that
			//    one too if it was finished and waiting (wedged until now).
			do {
				uint8_t prev = static_cast<uint8_t>((y - 1) & mask);
				res.end[prev] = res.end[y];
				y = prev; --cnt;
			} while (cnt > 1 && (done & (1u << y)) && ((done = static_cast<uint8_t>(done & ~(1u << y))), true));
			res.done = done; res.count = cnt;
		} else if (slot == first) {
			// 2. the oldest finished: release it and every finished one behind it.
			uint16_t h;
			do {
				h     = res.end[first];
				done  = static_cast<uint8_t>(done & ~(1u << first));
				first = static_cast<uint8_t>((first + 1) & mask);
				--cnt;
			} while (done & (1u << first));                  // the youngest is never done: stops in time
			res.done = done; res.first = first; res.count = cnt;
			state.head = h;                                  // single STRH: publish
		} else {
			// 3. one in the middle finished: it waits, wedged between two open ones.
			res.done = static_cast<uint8_t>(done | (1u << slot));
		}
	}
	// Under ProducerGuard, res.count != 0. Appends n elements that are already
	// written to the youngest table entry (open or finished): they are published
	// together with it. Takes no table entry, so it cannot be refused by the table.
	RB_OPT_INLINE constexpr void res_extend(size_t n) {
		release_barrier();
		res.reserved = static_cast<uint16_t>(wrapSize(res.reserved + static_cast<uint32_t>(n)));
		res.end[(res.first + res.count - 1) & (max_reservations - 1)] = res.reserved;
	}

	// Slow paths of push()/push_n() while a reservation is open: the data is
	// complete as soon as it is written, so it is simply appended to the youngest
	// table entry and published with it - never ahead of an unfinished
	// reservation, and limited by free space only. Called with ProducerGuard
	// already held (no nested locking). Out of line: see RB_OPT_COLD.
	// item by value: a reference would force push() to keep the element in memory
	// (address escapes into the out-of-line call) even on the fast path.
	RB_OPT_COLD constexpr bool push_behind_reservations(T item, uint32_t tail) {
		uint16_t r = res.reserved;
		if (nextIndex(r) == tail) return false;
		acquire_barrier();
		buffer[r] = item;
		res_extend(1);
		return true;
	}
	RB_OPT_COLD constexpr size_t push_n_behind_reservations(const T* ptr, size_t count, uint32_t tail) {
		uint16_t r = res.reserved;
		size_t free_r = wrapSize(tail + Size - (uint32_t)r - 1);
		if (count > free_r) count = free_r;
		if (count == 0) return 0;
		size_t first_r = Size - r;
		if (first_r > count) first_r = count;
		acquire_barrier();
		if (copy_with_memcpy(count)) {
			copy_memcpy(&buffer[r], ptr,           first_r);
			copy_memcpy(&buffer[0], ptr + first_r, count - first_r);
		} else {
			copy_loop(&buffer[r], ptr,           first_r);
			copy_loop(&buffer[0], ptr + first_r, count - first_r);
		}
		res_extend(count);                                   // may wrap: the end index wraps too
		return count;
	}

	public:
	using value_type = T;
	static constexpr size_t capacity = Size - 1;

	// Returned by reserve(): write `count` elements to `ptr`, then commit().
	// count == 0: refused (buffer full or max_reservations already open).
	struct Reservation {
		T*       ptr;
		uint16_t count;
		uint8_t  slot;               // internal
	};

	constexpr RingBuffer_PackedState() : state{}, res{}, buffer{}
	{
		if constexpr (std::is_same_v<IrqPolicy, Topology::Unspecified_<>>)
			RingBuffer_PackedState_topology_not_specified<IrqPolicy>();
		if constexpr (!_IsTestInstance)
		{
			// The test runs on uint8_t elements, not on T: what it checks is the index
			// logic, which does not depend on the element type - and running it on T
			// would demand that T can be built from and compared with integers, which
			// ruled out structs, enums, pointers and bool. conditional_t only makes the
			// type depend on T, so the assert is evaluated when the constructor is
			// instantiated (the class is complete then), not when the template is parsed.
			using test_type = RingBuffer_PackedState<std::conditional_t<std::is_void_v<T>, T, uint8_t>, 4, Topology::None<>, true>;
			static_assert(unit_test_ringbuffer<test_type>::run_test(), "RingBuffer_PackedState unit test failed!");
			// Reservation tests only where reservations are enabled - no compile
			// time for anybody else.
			if constexpr (max_reservations != 0) {
				using res_test_type = RingBuffer_PackedState<std::conditional_t<std::is_void_v<T>, T, uint8_t>, 8,   // uint8_t: as test_type
					RingBuffer_PackedState_ConstexprReservationTestPolicy, true>;
				static_assert(unit_test_reservations<res_test_type>::run_test(),
				              "RingBuffer_PackedState reservation unit test failed!");
			}
		}
	}

	RB_OPT_INLINE constexpr bool push(const T& item) {
		ProducerGuard g;
		auto s          = readHT();
		if constexpr (max_reservations != 0) {
			if (res.count == 0) [[likely]] {             // no reservation open: fast path
				uint16_t next_h = nextIndex(s.head);
				if (next_h == s.tail) [[unlikely]] return false;
				acquire_barrier();
				buffer[s.head] = item;
				release_barrier();
				state.head   = next_h;                   // single STRH
				return true;
			}
			return push_behind_reservations(item, s.tail);   // older reservation open: slow
		}                                                // path out of line, same lock
		uint16_t next_h = nextIndex(s.head);
		if (next_h == s.tail) [[unlikely]] return false; // full
		acquire_barrier();
		buffer[s.head] = item;
		release_barrier();
		state.head = next_h;                             // single STRH
		return true;
	}

	RB_OPT_INLINE constexpr bool pop(T& item) {
		ConsumerGuard g;
		uint32_t w = readRaw();
		if (rawEmpty(w)) [[unlikely]] return false;      // empty
		uint16_t t = static_cast<uint16_t>(w >> 16);
		acquire_barrier();
		item = buffer[t];
		release_barrier();
		state.tail = nextIndex(t);                       // single STRH
		return true;
	}

	// Returns the next element without advancing tail. Uses one 32-bit snapshot
	// of head and tail, so the empty check and read index are always consistent.
	RB_OPT_INLINE constexpr bool peek(T& item) const {
		uint32_t w = readRaw();
		if (rawEmpty(w)) [[unlikely]] return false;
		acquire_barrier();
		item = buffer[static_cast<uint16_t>(w >> 16)];
		return true;
	}

	// Returns the element at the given offset from tail without advancing tail.
	// offset=0 is equivalent to peek(item) — the next element to be popped.
	// Returns false if offset is beyond the number of available elements.
	RB_OPT_INLINE constexpr bool peek(T& item, size_t offset) const {
		auto s = readHT();
		size_t available = wrapSize((uint32_t)s.head - (uint32_t)s.tail + Size);
		if (offset >= available) [[unlikely]] return false;
		size_t idx = wrapSize((uint32_t)s.tail + offset);
		acquire_barrier();
		item = buffer[idx];
		return true;
	}

	// Pushes up to count elements from ptr. Returns the number actually pushed —
	// may be less than count if space is limited. Handles wrap automatically.
	// ProducerGuard protected. Copy: see copy_with_memcpy() / RB_COPY_LOOP_MAX.
	RB_OPT_COPY constexpr size_t push_n(const T* ptr, size_t count) {
		ProducerGuard g;
		auto [h, t] = readHT();
		if constexpr (max_reservations != 0) {
			if (res.count != 0) [[unlikely]]             // older reservation open: as push()
				return push_n_behind_reservations(ptr, count, t);
		}
		size_t available = wrapSize((uint32_t)t + Size - (uint32_t)h - 1);
		if (count > available) count = available;
		size_t first = Size - h;
		if (first > count) first = count;
		acquire_barrier();
		if (copy_with_memcpy(count)) {
			copy_memcpy(&buffer[h], ptr,         first);
			copy_memcpy(&buffer[0], ptr + first, count - first);
		} else {
			copy_loop(&buffer[h], ptr,         first);
			copy_loop(&buffer[0], ptr + first, count - first);
		}
		release_barrier();
		uint32_t h32 = (uint32_t)h + count;
		if constexpr (is_power_of_two)
			h = static_cast<uint16_t>(h32 & (Size - 1));
		else
			h = static_cast<uint16_t>(h32 >= Size ? h32 - Size : h32);
		state.head = h;                                  // single STRH
		return count;
	}

	// Pops up to count elements into ptr. Returns the number actually popped —
	// may be less than count if fewer elements are available. Handles wrap automatically.
	// ConsumerGuard protected. Copy strategy as in push_n().
	RB_OPT_COPY constexpr size_t pop_n(T* ptr, size_t count) {
		ConsumerGuard g;
		auto [h, t] = readHT();
		size_t available = wrapSize((uint32_t)h - (uint32_t)t + Size);
		if (count > available) count = available;
		size_t first = Size - t;
		if (first > count) first = count;
		acquire_barrier();
		if (copy_with_memcpy(count)) {
			copy_memcpy(ptr,         &buffer[t], first);
			copy_memcpy(ptr + first, &buffer[0], count - first);
		} else {
			copy_loop(ptr,         &buffer[t], first);
			copy_loop(ptr + first, &buffer[0], count - first);
		}
		release_barrier();
		uint32_t t32 = (uint32_t)t + count;
		if constexpr (is_power_of_two)
			t = static_cast<uint16_t>(t32 & (Size - 1));
		else
			t = static_cast<uint16_t>(t32 >= Size ? t32 - Size : t32);
		state.tail = t;                                  // single STRH
		return count;
	}

	RB_OPT_INLINE constexpr bool   isFull()   const { auto s = readHT(); return nextIndex(s.head) == s.tail; }
	RB_OPT_INLINE constexpr bool   isEmpty()  const { return rawEmpty(readRaw()); }

	// Resets the buffer to empty (head = tail = 0) with ONE 32-bit store, so no
	// other context can ever observe a half-cleared state — no IRQ lock needed
	// for that. A lock is taken only in topologies where a side already locks
	// (MPSC/SPMC/MPMC), so clear() also waits out an in-progress guarded op
	// when LockImpl is a preemptible lock (e.g. an RTOS mutex).
	//
	// Call contract — clear() overwrites the OTHER side's field too:
	//   SAFE   from the lowest-priority context sharing the buffer (typically
	//          main): an ISR on the other side runs to completion, so it can
	//          never be caught mid-operation by the store.
	//   SAFE   whenever the other side is quiescent (init, ISR disabled).
	//   UNSAFE from an ISR that can preempt an UNGUARDED side mid-push/pop:
	//          that side then writes its stale index back after the clear and
	//          corrupts the count. No lock inside clear() can prevent this,
	//          because the unguarded side never takes the lock.
	// Not safe while DMA is active on the buffer; stop the DMA first.
	// With reservations enabled: no reservation may be open (its owner would
	// later commit into the reset table).
	RB_OPT_INLINE constexpr void clear() {
		if (RB_IS_CONSTANT_EVALUATED()) {
			if constexpr (max_reservations != 0) res = ResState<max_reservations>{};
			state.head = 0; state.tail = 0; return;
		}
		ClearGuard g;
		if constexpr (max_reservations != 0) res = ResState<max_reservations>{};   // under the lock
		release_barrier();   // finish element accesses before slots are handed back
		typedef uint32_t __attribute__((may_alias)) u32_alias;
		if constexpr (IrqPolicy::needs_volatile)
			*reinterpret_cast<volatile u32_alias*>(&state) = 0;   // single STR
		else
			*reinterpret_cast<u32_alias*>(&state) = 0;
	}

	RB_OPT_INLINE constexpr size_t getCount() const {
		auto s = readHT();
		if constexpr (is_power_of_two) return static_cast<size_t>((uint32_t)(s.head - s.tail) & (Size - 1));
		else                           return wrapSize((uint32_t)s.head - (uint32_t)s.tail + Size);
	}
	RB_OPT_INLINE constexpr size_t getSpace() const { return capacity - getCount(); }

	// Discards up to count elements without reading. Consumer operation.
	// Equivalent to commit_pop() — provided for clarity at call sites where
	// data is intentionally discarded rather than consumed after a DMA read.
	RB_OPT_INLINE constexpr void skip(size_t count) { commit_pop(count); }

	struct ContiguousArea {
		element_type* ptr;
		size_t count;
	};

	struct ConstContiguousArea {
		const element_type* ptr;
		size_t count;
	};

	// Returns pointer to head and how many elements can be written contiguously
	// before the buffer wraps. Call commit_push() after writing.
	// NOTE: reads state without a Guard — only one producer may call this at a time.
	RB_OPT_INLINE constexpr ContiguousArea get_contiguous_push_area(size_t max_size) {
		static_assert(max_reservations == 0, "RingBuffer_PackedState: with reservations enabled use "
		              "reserve()/commit() - get_contiguous_push_area()/commit_push() bypass the reservation table");
		auto [h, t] = readHT();
		size_t   available = wrapSize((uint32_t)t + Size - (uint32_t)h - 1);
		size_t   contiguous = Size - h;
		if (contiguous > available) contiguous = available;
		if (contiguous > max_size)  contiguous = max_size;
		acquire_barrier();                                   // caller's writes stay after the snapshot
		return { &buffer[h], contiguous };
	}

	// Advances head by count after a contiguous push
	RB_OPT_INLINE constexpr void commit_push(size_t count) {
		static_assert(max_reservations == 0, "RingBuffer_PackedState: with reservations enabled use "
		              "reserve()/commit() - get_contiguous_push_area()/commit_push() bypass the reservation table");
		ProducerGuard g;
		auto [h, t] = readHT();
		size_t   available = wrapSize((uint32_t)t + Size - (uint32_t)h - 1);
		if (count > available) count = available;
		uint32_t h32 = (uint32_t)h + count;
		if constexpr (is_power_of_two)
			h = static_cast<uint16_t>(h32 & (Size - 1));
		else
			h = static_cast<uint16_t>(h32 >= Size ? h32 - Size : h32);
		release_barrier();                                   // caller's writes complete before publish
		state.head = h;                                      // single STRH
	}

	// Returns pointer to tail and how many elements can be read contiguously
	// before the buffer wraps. Call commit_pop() after consuming.
	// NOTE: reads state without a Guard — only one consumer may call this at a time.
	RB_OPT_INLINE constexpr ConstContiguousArea get_contiguous_pop_area(size_t max_size) const {
		auto [h, t] = readHT();
		size_t   available = wrapSize((uint32_t)h - (uint32_t)t + Size);
		size_t   contiguous = Size - t;
		if (contiguous > available) contiguous = available;
		if (contiguous > max_size)  contiguous = max_size;
		acquire_barrier();                                   // caller's reads stay after the snapshot
		return { &buffer[t], contiguous };
	}

	// Advances tail by count after a contiguous pop
	RB_OPT_INLINE constexpr void commit_pop(size_t count) {
		ConsumerGuard g;
		auto [h, t] = readHT();
		size_t   available = wrapSize((uint32_t)h - (uint32_t)t + Size);
		if (count > available) count = available;
		uint32_t t32 = (uint32_t)t + count;
		if constexpr (is_power_of_two)
			t = static_cast<uint16_t>(t32 & (Size - 1));
		else
			t = static_cast<uint16_t>(t32 >= Size ? t32 - Size : t32);
		release_barrier();                                   // caller's reads complete before publish
		state.tail = t;                                      // single STRH
	}

	// Reserves up to max_size contiguous push slots, advances head immediately,
	// and returns pointer + count. Write to ptr after the call — no commit needed.
	// CAUTION: the head is published BEFORE the data is written, so a consumer
	// running concurrently can read the slots too early. Only safe while the
	// consumer cannot run (e.g. its ISR disabled until the data is complete).
	// For concurrent producers that fill outside the lock use the reservation
	// API instead: Topology::MPSC/MPMC<Lock, N> with reserve() + commit().
	RB_OPT constexpr ContiguousArea reserve_push(size_t max_size) {
		static_assert(max_reservations == 0, "RingBuffer_PackedState: with reservations enabled use "
		              "reserve()/commit() - reserve_push() publishes before the data is written");
		ProducerGuard g;
		auto [h, t] = readHT();
		size_t   available  = wrapSize((uint32_t)t + Size - (uint32_t)h - 1);
		size_t   contiguous = Size - h;
		if (contiguous > available) contiguous = available;
		if (contiguous > max_size)  contiguous = max_size;
		element_type* ptr = &buffer[h];
		uint32_t h32 = (uint32_t)h + contiguous;
		if constexpr (is_power_of_two)
			h = static_cast<uint16_t>(h32 & (Size - 1));
		else
			h = static_cast<uint16_t>(h32 >= Size ? h32 - Size : h32);
		state.head = h;                                      // single STRH
		return { ptr, contiguous };
	}

	// Reserves up to max_size contiguous pop slots, advances tail immediately,
	// and returns pointer + count. Read from ptr after the call — no commit needed.
	// CAUTION: the tail is published BEFORE the data is read, so a producer
	// running concurrently can overwrite the slots while they are still being
	// read. Only safe while the producer cannot run.
	RB_OPT constexpr ConstContiguousArea reserve_pop(size_t max_size) {
		ConsumerGuard g;
		auto [h, t] = readHT();
		size_t   available  = wrapSize((uint32_t)h - (uint32_t)t + Size);
		size_t   contiguous = Size - t;
		if (contiguous > available) contiguous = available;
		if (contiguous > max_size)  contiguous = max_size;
		const element_type* ptr = &buffer[t];
		uint32_t t32 = (uint32_t)t + contiguous;
		if constexpr (is_power_of_two)
			t = static_cast<uint16_t>(t32 & (Size - 1));
		else
			t = static_cast<uint16_t>(t32 >= Size ? t32 - Size : t32);
		state.tail = t;                                      // single STRH
		return { ptr, contiguous };
	}

	// ---- producer reservations: Topology::MPSC/MPMC<Lock, N>, N = 1, 2, 4, 8 ----
	//
	// reserve(max): hands out up to max CONTIGUOUS free slots (stops at the array
	//   end, like get_contiguous_push_area). Returns count == 0 when the buffer is
	//   full or the reservation table is full - nothing waits.
	// Fill r.ptr[0 .. r.count) in any context, outside any lock (copy, DMA), then
	// commit(r): the data becomes visible once every OLDER reservation is
	// committed too; reservations may finish in any order.
	// commit(r, used): as commit(r), but publishes only the first `used` elements
	//   when the block can still shrink - see there.
	// push()/push_n() keep working: behind an open reservation they queue up after
	// it, take no table entry and are limited by free space only.
	// The table has N entries. An entry is taken by every open reservation, and by
	// a finished one for as long as an OLDER and a YOUNGER reservation are both
	// still open (it is wedged between them). With up to N reservations open at
	// the same time reserve() is therefore never refused by the table as long as
	// they are committed youngest-first or oldest-first (e.g. nested ISRs);
	// committing one in the middle first can cost an entry until a neighbour ends.
	// Contract: every non-empty reservation must be committed exactly once; an
	// uncommitted one holds back everything reserved after it. Do not clear()
	// while a reservation is open. Neither is detected: a second commit, or one
	// after clear(), can publish another producer's unfinished reservation.
	// isFull()/getSpace() count committed data only; producers should rely on the
	// return values of reserve()/push()/push_n().
	RB_OPT constexpr Reservation reserve(size_t max_size) {
		static_assert(max_reservations != 0, "RingBuffer_PackedState: reserve()/commit() need reservations "
		              "enabled - Topology::MPSC<PrimaskLock, N> or MPMC<PrimaskLock, N>, N = 1, 2, 4 or 8");
		ProducerGuard g;
		auto [h, t] = readHT();
		// res.reserved is only maintained while a reservation is open; with none
		// open the next free slot is the committed head (saves push() a store).
		uint16_t r = (res.count != 0) ? res.reserved : h;
		size_t free_r = wrapSize((uint32_t)t + Size - (uint32_t)r - 1);
		size_t n = Size - r;                                 // contiguous to the array end
		if (n > free_r)   n = free_r;
		if (n > max_size) n = max_size;
		if (n == 0 || res.count == max_reservations) return { nullptr, 0, 0 };
		uint8_t slot = res_alloc(r, n);
		acquire_barrier();                                   // caller's writes stay after this
		return { &buffer[r], static_cast<uint16_t>(n), slot };
	}

	RB_OPT constexpr void commit(const Reservation& rv) {
		static_assert(max_reservations != 0, "RingBuffer_PackedState: reserve()/commit() need reservations "
		              "enabled - Topology::MPSC<PrimaskLock, N> or MPMC<PrimaskLock, N>, N = 1, 2, 4 or 8");
		if (rv.count == 0) return;                           // refused reservation: nothing to do
		ProducerGuard g;
		res_finish(rv.slot);                                 // starts with the release barrier
	}

	// commit(r, used): finishes reservation r like commit(r), but publishes only
	// its first `used` elements IF nothing has been reserved or pushed behind r in
	// the meantime - the rest goes back to the free space. Otherwise the slots
	// behind r are taken, the block cannot shrink, and all r.count elements are
	// published. Returns the number published (used, or r.count).
	// The caller cannot know which case it gets (another producer may have come in
	// between), so fill the unused tail with something the consumer can skip
	// BEFORE the call. That fits framed data (padding, an empty record), not a raw
	// byte stream. With a single producer nothing can come in between: it always
	// shrinks. used = 0 gives the reservation back completely when it can.
	RB_OPT constexpr size_t commit(const Reservation& rv, size_t used) {
		static_assert(max_reservations != 0, "RingBuffer_PackedState: reserve()/commit() need reservations "
		              "enabled - Topology::MPSC<PrimaskLock, N> or MPMC<PrimaskLock, N>, N = 1, 2, 4 or 8");
		if (rv.count == 0) return 0;
		ProducerGuard g;
		size_t   n     = rv.count;
		uint32_t start = static_cast<uint32_t>(rv.ptr - buffer);
		// Nothing allocated behind r  <=>  the next free slot is still r's end. r is
		// then the youngest table entry (res_finish leaves no empty entry behind).
		if (used < n && res.reserved == wrapSize(start + static_cast<uint32_t>(n))) {
			res.reserved     = static_cast<uint16_t>(wrapSize(start + static_cast<uint32_t>(used)));
			res.end[rv.slot] = res.reserved;
			n = used;
		}
		res_finish(rv.slot);
		return n;
	}
};

#undef RB_OPT
#undef RB_OPT_INLINE
#undef RB_OPT_COPY
#undef RB_OPT_COLD
#undef RB_IS_CONSTANT_EVALUATED
#undef RB_ASSUME
