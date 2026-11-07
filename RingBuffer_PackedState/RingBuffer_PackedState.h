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

inline constexpr const char* RINGBUFFER_PACKEDSTATE_VERSION = "1.3.3";

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

#if defined(__GNUC__) || defined(__clang__)
	#define RB_OPT        [[gnu::optimize("Os")]]
	#define RB_OPT_INLINE [[gnu::optimize("Os"), gnu::always_inline]]
#else
	#define RB_OPT
	#define RB_OPT_INLINE
#endif

// The builtin folds to a constant even at -O0. std::is_constant_evaluated() is
// a libstdc++ wrapper that, in -O0 Debug builds, is emitted as a real
// `bl std::is_constant_evaluated` + compare + branch at every inlined use.
#if defined(__GNUC__) || defined(__clang__)
	#define RB_IS_CONSTANT_EVALUATED() __builtin_is_constant_evaluated()
#else
	#define RB_IS_CONSTANT_EVALUATED() std::is_constant_evaluated()
#endif


struct PrimaskLock {
	static uint32_t lock()             { uint32_t p = __get_PRIMASK(); __disable_irq(); return p; }
	static void     unlock(uint32_t p) { __set_PRIMASK(p); }
};

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
	template<typename LockImpl = PrimaskLock>
	struct MPSC : LockImpl {
		static constexpr bool needs_volatile = true;
		static constexpr bool lock_p_needed  = true;
		static constexpr bool lock_c_needed  = false;
	};

	// Single Producer, Multiple Consumers — consumer side guarded, producer side free.
	template<typename LockImpl = PrimaskLock>
	struct SPMC : LockImpl {
		static constexpr bool needs_volatile = true;
		static constexpr bool lock_p_needed  = false;
		static constexpr bool lock_c_needed  = true;
	};

	// Multiple Producers, Multiple Consumers — both sides guarded.
	template<typename LockImpl = PrimaskLock>
	struct MPMC : LockImpl {
		static constexpr bool needs_volatile = true;
		static constexpr bool lock_p_needed  = true;
		static constexpr bool lock_c_needed  = true;
	};
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

// Optimised ring buffer with head and tail as adjacent volatile uint16_t fields.
// Reads are LDRH, writes are STRH — in SPSC each side owns one field exclusively.
// One slot is sacrificed to distinguish full from empty without a count variable.
// Effective capacity is Size-1.
template<typename T, size_t Size, typename IrqPolicy = Topology::None<>, bool _IsTestInstance = false>
class RingBuffer_PackedState
{
	static_assert(Size >= 2 && Size <= 65535, "RingBuffer_PackedState: Size must be in range [2, 65535]");

	private:
	template<bool Active>
	struct GuardImpl {
		uint32_t primask;
		static constexpr uint32_t do_lock() {
			if constexpr (Active) return IrqPolicy::lock();
			else return 0;
		}
		constexpr GuardImpl()  : primask(do_lock()) {}
		constexpr ~GuardImpl() { if constexpr (Active) IrqPolicy::unlock(primask); }
	};
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
	// accesses — and volatile on the elements could not give that to memcpy or to
	// user code writing through a zero-copy pointer with volatile cast away.
	// The order is enforced by acquire_barrier() / release_barrier() below.
	using element_type = T;

	// Adjacent halfwords — head written by producer (STRH), tail written by consumer (STRH).
	alignas(4) struct { half_type head; half_type tail; } state;
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
	RB_OPT_INLINE constexpr StateSnapshot readHT() const {
		if (RB_IS_CONSTANT_EVALUATED()) {
			return { static_cast<uint16_t>(state.head), static_cast<uint16_t>(state.tail) };
		}
		typedef uint32_t __attribute__((may_alias)) u32_alias;
		uint32_t raw;
		if constexpr (IrqPolicy::needs_volatile) {
			raw = *reinterpret_cast<const volatile u32_alias*>(&state);
		} else {
			raw = *reinterpret_cast<const u32_alias*>(&state);
		}
		return { static_cast<uint16_t>(raw), static_cast<uint16_t>(raw >> 16) };
	}

	RB_OPT_INLINE constexpr uint16_t nextIndex(uint16_t idx) const {
		if constexpr (is_power_of_two) return (idx + 1) & static_cast<uint16_t>(Size - 1);
		else                           return (idx + 1 >= static_cast<uint16_t>(Size)) ? static_cast<uint16_t>(0) : static_cast<uint16_t>(idx + 1);
	}

	RB_OPT_INLINE constexpr size_t wrapSize(uint32_t val) const {
		if constexpr (is_power_of_two) return val & (Size - 1);
		else                           return (val >= Size) ? val - Size : val;
	}

	public:
	using value_type = T;
	static constexpr size_t capacity = Size - 1;

	constexpr RingBuffer_PackedState() : state{}, buffer{}
	{
		if constexpr (!_IsTestInstance)
		{
			using test_type = RingBuffer_PackedState<T, 4, Topology::None<>, true>;
			static_assert(unit_test_ringbuffer<test_type>::run_test(), "RingBuffer_PackedState unit test failed!");
		}
	}

	RB_OPT_INLINE constexpr bool push(const T& item) {
		ProducerGuard g;
		auto s          = readHT();
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
		auto s = readHT();
		if (s.head == s.tail) [[unlikely]] return false; // empty
		acquire_barrier();
		item = buffer[s.tail];
		release_barrier();
		state.tail = nextIndex(s.tail);                  // single STRH
		return true;
	}

	// Returns the next element without advancing tail. Uses readHT() for an atomic
	// snapshot of head and tail, so the empty check and read index are always consistent.
	RB_OPT_INLINE constexpr bool peek(T& item) const {
		auto s = readHT();
		if (s.head == s.tail) [[unlikely]] return false;
		acquire_barrier();
		item = buffer[s.tail];
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
	// ProducerGuard protected.
	RB_OPT constexpr size_t push_n(const T* ptr, size_t count) {
		ProducerGuard g;
		auto [h, t] = readHT();
		size_t available = wrapSize((uint32_t)t + Size - (uint32_t)h - 1);
		if (count > available) count = available;
		size_t first = Size - h;
		if (first > count) first = count;
		acquire_barrier();
		if constexpr (std::is_trivially_copyable_v<T>) {
			memcpy(&buffer[h], ptr,         first           * sizeof(T));
			memcpy(&buffer[0], ptr + first, (count - first) * sizeof(T));
		} else {
			for (size_t i = 0; i < first; ++i)        buffer[h + i] = ptr[i];
			for (size_t i = 0; i < count - first; ++i) buffer[i]    = ptr[first + i];
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
	// ConsumerGuard protected.
	RB_OPT constexpr size_t pop_n(T* ptr, size_t count) {
		ConsumerGuard g;
		auto [h, t] = readHT();
		size_t available = wrapSize((uint32_t)h - (uint32_t)t + Size);
		if (count > available) count = available;
		size_t first = Size - t;
		if (first > count) first = count;
		acquire_barrier();
		if constexpr (std::is_trivially_copyable_v<T>) {
			memcpy(ptr,         &buffer[t], first           * sizeof(T));
			memcpy(ptr + first, &buffer[0], (count - first) * sizeof(T));
		} else {
			for (size_t i = 0; i < first; ++i)        ptr[i]        = buffer[t + i];
			for (size_t i = 0; i < count - first; ++i) ptr[first + i] = buffer[i];
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
	RB_OPT_INLINE constexpr bool   isEmpty()  const { auto s = readHT(); return s.head == s.tail; }

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
	RB_OPT_INLINE constexpr void clear() {
		if (RB_IS_CONSTANT_EVALUATED()) { state.head = 0; state.tail = 0; return; }
		ClearGuard g;
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
	void skip(size_t count) { commit_pop(count); }

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
	RB_OPT constexpr ContiguousArea get_contiguous_push_area(size_t max_size) {
		auto [h, t] = readHT();
		size_t   available = wrapSize((uint32_t)t + Size - (uint32_t)h - 1);
		size_t   contiguous = Size - h;
		if (contiguous > available) contiguous = available;
		if (contiguous > max_size)  contiguous = max_size;
		acquire_barrier();                                   // caller's writes stay after the snapshot
		return { &buffer[h], contiguous };
	}

	// Advances head by count after a contiguous push
	RB_OPT constexpr void commit_push(size_t count) {
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
	RB_OPT constexpr ConstContiguousArea get_contiguous_pop_area(size_t max_size) const {
		auto [h, t] = readHT();
		size_t   available = wrapSize((uint32_t)h - (uint32_t)t + Size);
		size_t   contiguous = Size - t;
		if (contiguous > available) contiguous = available;
		if (contiguous > max_size)  contiguous = max_size;
		acquire_barrier();                                   // caller's reads stay after the snapshot
		return { &buffer[t], contiguous };
	}

	// Advances tail by count after a contiguous pop
	RB_OPT constexpr void commit_pop(size_t count) {
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
	RB_OPT constexpr ContiguousArea reserve_push(size_t max_size) {
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
};

#undef RB_OPT
#undef RB_OPT_INLINE
#undef RB_IS_CONSTANT_EVALUATED
