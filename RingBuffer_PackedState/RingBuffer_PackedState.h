/*
 * RingBuffer_PackedState.h
 *
 *  Created on: 18. apr. 2026
 *      Author: Erik Nørskov
 *
 *  Copyright (c) 2026 PxQ Technologies
 *  https://pxq.dk
 *
 *  Dual License:
 *
 *  1. Open Source / Non-Commercial Use:
 *     This file is free to use, modify, and distribute under the terms of the
 *     MIT License, provided it is used solely in open source or non-commercial
 *     projects. The above copyright notice must be retained in all copies.
 *
 *  2. Commercial Use:
 *     Use of this file in a commercial or proprietary product requires either:
 *     a) A separate written commercial license agreement with PxQ Technologies, or
 *     b) Direct delivery of this file by Erik Nørskov (PxQ Technologies) as part
 *        of a paid engagement or employment, in which case a license is granted
 *        for use within that specific project scope only.
 *
 *     Contact: https://pxq.dk
 */

#pragma once

inline constexpr const char* RINGBUFFER_PACKEDSTATE_VERSION = "1.2.1";

#include <cstdint>
#include <cstddef>
#include <type_traits>

#if defined(__GNUC__) || defined(__clang__)
	#define RB_OPT        [[gnu::optimize("Os")]]
	#define RB_OPT_INLINE [[gnu::optimize("Os"), gnu::always_inline]]
#else
	#define RB_OPT
	#define RB_OPT_INLINE
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

	static constexpr bool is_power_of_two = (Size & (Size - 1)) == 0;

	using half_type    = std::conditional_t<IrqPolicy::needs_volatile, volatile uint16_t, uint16_t>;
	using element_type = std::conditional_t<IrqPolicy::needs_volatile, volatile T, T>;

	// Adjacent halfwords — head written by producer (STRH), tail written by consumer (STRH).
	alignas(4) struct { half_type head; half_type tail; } state;
	alignas(alignof(T)) element_type buffer[Size];

	// Non-volatile snapshot — same layout as state, always uint16_t regardless of IrqPolicy.
	struct StateSnapshot { uint16_t head; uint16_t tail; };

	// Atomic snapshot of head and tail. At runtime: single volatile LDR into StateSnapshot
	// (state is alignas(4), same layout). At compile time: two separate reads
	// (reinterpret_cast not allowed in constexpr).
	RB_OPT_INLINE constexpr StateSnapshot readHT() const {
		if (std::is_constant_evaluated()) {
			return { static_cast<uint16_t>(state.head), static_cast<uint16_t>(state.tail) };
		}
		StateSnapshot snap;
		*reinterpret_cast<uint32_t*>(&snap) = *reinterpret_cast<const volatile uint32_t*>(&state);
		return snap;
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
		if (next_h == s.tail) return false;              // full
		buffer[s.head] = item;
		state.head = next_h;                             // single STRH
		return true;
	}

	RB_OPT_INLINE constexpr bool pop(T& item) {
		ConsumerGuard g;
		auto s = readHT();
		if (s.head == s.tail) return false;              // empty
		item = buffer[s.tail];
		state.tail = nextIndex(s.tail);                  // single STRH
		return true;
	}

	// Returns the next element without advancing tail. Uses readHT() for an atomic
	// snapshot of head and tail, so the empty check and read index are always consistent.
	RB_OPT_INLINE constexpr bool peek(T& item) const {
		auto s = readHT();
		if (s.head == s.tail) return false;
		item = static_cast<T>(buffer[s.tail]);
		return true;
	}

	RB_OPT_INLINE constexpr bool   isFull()   const { auto s = readHT(); return nextIndex(s.head) == s.tail; }
	RB_OPT_INLINE constexpr bool   isEmpty()  const { auto s = readHT(); return s.head == s.tail; }
	RB_OPT_INLINE constexpr size_t getCount() const { auto s = readHT(); return wrapSize((uint32_t)s.head - (uint32_t)s.tail + Size); }

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
