/*
 * RingBuffer_32bit.h
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

// Template specialization trick: GCC emits [[deprecated]] warnings even inside
// if constexpr (false) branches. Using a struct specialization fires the warning
// at type instantiation level instead, which GCC handles correctly.
namespace detail {
	template<bool> struct NonPow2Checker {
		static constexpr void check() {}
	};
	template<> struct NonPow2Checker<true> {
		[[deprecated("RingBuffer_32Bit: Size is not a power of 2 — consider a power-of-2 size for optimal performance (suppress with WarnNonPow2=false)")]]
		static constexpr void check() {}
	};
}

struct NoIrqProtection {
	static constexpr bool needs_volatile = false;
	static constexpr void lock()   {}
	static constexpr void unlock() {}
};

struct IrqProtection {
	static constexpr bool needs_volatile = true;
	static void lock()   { __disable_irq(); }
	static void unlock() { __enable_irq(); }
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

// Optimised ring buffer using head+tail packed into a single uint32_t for
// atomic LDR/STR on Cortex-M. Head in low 16 bits, tail in high 16 bits.
// One slot is sacrificed to distinguish full from empty without a count variable.
// Effective capacity is Size-1.
template<typename T, size_t Size, bool WarnNonPow2 = true, typename IrqPolicy = NoIrqProtection, bool _IsTestInstance = false>
class RingBuffer_32Bit
{
	static_assert(Size >= 2 && Size <= 65535, "RingBuffer_32Bit: Size must be in range [2, 65535]");

	private:
	struct Guard {
		constexpr Guard()  { IrqPolicy::lock(); }
		constexpr ~Guard() { IrqPolicy::unlock(); }
	};

	static constexpr bool is_power_of_two = (Size & (Size - 1)) == 0;

	using state_type   = std::conditional_t<IrqPolicy::needs_volatile, volatile uint32_t, uint32_t>;
	using element_type = std::conditional_t<IrqPolicy::needs_volatile, volatile T, T>;

	// head in low 16 bits, tail in high 16 bits — single LDR/STR on Cortex-M
	alignas(4) state_type state;
	alignas(alignof(T)) element_type buffer[Size];

	RB_OPT_INLINE constexpr uint16_t getHead(uint32_t s)  const { return static_cast<uint16_t>(s); }
	RB_OPT_INLINE constexpr uint16_t getTail(uint32_t s)  const { return static_cast<uint16_t>(s >> 16); }
	RB_OPT_INLINE constexpr uint32_t pack(uint16_t head, uint16_t tail) const { return static_cast<uint32_t>(head) | (static_cast<uint32_t>(tail) << 16); }

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

	constexpr RingBuffer_32Bit() : state{0}, buffer{}
	{
		detail::NonPow2Checker<!is_power_of_two && WarnNonPow2>::check();

		if constexpr (!_IsTestInstance)
		{
			using test_type = RingBuffer_32Bit<T, 4, true, NoIrqProtection, true>;
			static_assert(unit_test_ringbuffer<test_type>::run_test(), "RingBuffer_32Bit unit test failed!");
		}
	}

	RB_OPT_INLINE constexpr bool push(const T& item) {
		Guard g;
		uint32_t s      = static_cast<uint32_t>(state); // single LDR
		uint16_t h      = getHead(s);
		uint16_t t      = getTail(s);
		uint16_t next_h = nextIndex(h);
		if (next_h == t) return false;                   // full
		buffer[h] = item;
		state = pack(next_h, t);                         // single STR
		return true;
	}

	RB_OPT_INLINE constexpr bool pop(T& item) {
		Guard g;
		uint32_t s = static_cast<uint32_t>(state);       // single LDR
		uint16_t h = getHead(s);
		uint16_t t = getTail(s);
		if (h == t) return false;                         // empty
		item  = buffer[t];
		state = pack(h, nextIndex(t));                    // single STR
		return true;
	}

	// Single LDR gives atomic snapshot of both fields — no Guard needed for reads
	RB_OPT_INLINE constexpr bool   isFull()   const { uint32_t s = static_cast<uint32_t>(state); return nextIndex(getHead(s)) == getTail(s); }
	RB_OPT_INLINE constexpr bool   isEmpty()  const { uint32_t s = static_cast<uint32_t>(state); return getHead(s) == getTail(s); }
	RB_OPT_INLINE constexpr size_t getCount() const { uint32_t s = static_cast<uint32_t>(state); return wrapSize((uint32_t)getHead(s) - (uint32_t)getTail(s) + Size); }

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
		uint32_t s         = static_cast<uint32_t>(state);  // single LDR
		uint16_t h         = getHead(s);
		uint16_t t         = getTail(s);
		size_t   available = wrapSize((uint32_t)t + Size - (uint32_t)h - 1);
		size_t   contiguous = Size - h;
		if (contiguous > available) contiguous = available;
		if (contiguous > max_size)  contiguous = max_size;
		return { &buffer[h], contiguous };
	}

	// Advances head by count after a contiguous push
	RB_OPT constexpr void commit_push(size_t count) {
		Guard g;
		uint32_t s         = static_cast<uint32_t>(state);  // single LDR
		uint16_t h         = getHead(s);
		uint16_t t         = getTail(s);
		size_t   available = wrapSize((uint32_t)t + Size - (uint32_t)h - 1);
		if (count > available) count = available;
		if constexpr (is_power_of_two)
			h = static_cast<uint16_t>((h + count) & (Size - 1));
		else
			h = static_cast<uint16_t>((h + count) % Size);
		state = pack(h, t);                                  // single STR
	}

	// Returns pointer to tail and how many elements can be read contiguously
	// before the buffer wraps. Call commit_pop() after consuming.
	// NOTE: reads state without a Guard — only one consumer may call this at a time.
	RB_OPT constexpr ConstContiguousArea get_contiguous_pop_area(size_t max_size) const {
		uint32_t s         = static_cast<uint32_t>(state);  // single LDR
		uint16_t h         = getHead(s);
		uint16_t t         = getTail(s);
		size_t   available = wrapSize((uint32_t)h - (uint32_t)t + Size);
		size_t   contiguous = Size - t;
		if (contiguous > available) contiguous = available;
		if (contiguous > max_size)  contiguous = max_size;
		return { &buffer[t], contiguous };
	}

	// Advances tail by count after a contiguous pop
	RB_OPT constexpr void commit_pop(size_t count) {
		Guard g;
		uint32_t s         = static_cast<uint32_t>(state);  // single LDR
		uint16_t h         = getHead(s);
		uint16_t t         = getTail(s);
		size_t   available = wrapSize((uint32_t)h - (uint32_t)t + Size);
		if (count > available) count = available;
		if constexpr (is_power_of_two)
			t = static_cast<uint16_t>((t + count) & (Size - 1));
		else
			t = static_cast<uint16_t>((t + count) % Size);
		state = pack(h, t);                                  // single STR
	}

	// Reserves up to max_size contiguous push slots, advances head immediately,
	// and returns pointer + count. Write to ptr after the call — no commit needed.
	RB_OPT constexpr ContiguousArea reserve_push(size_t max_size) {
		Guard g;
		uint32_t s          = static_cast<uint32_t>(state); // single LDR
		uint16_t h          = getHead(s);
		uint16_t t          = getTail(s);
		size_t   available  = wrapSize((uint32_t)t + Size - (uint32_t)h - 1);
		size_t   contiguous = Size - h;
		if (contiguous > available) contiguous = available;
		if (contiguous > max_size)  contiguous = max_size;
		element_type* ptr = &buffer[h];
		if constexpr (is_power_of_two)
			h = static_cast<uint16_t>((h + contiguous) & (Size - 1));
		else
			h = static_cast<uint16_t>((h + contiguous) % Size);
		state = pack(h, t);                                  // single STR
		return { ptr, contiguous };
	}

	// Reserves up to max_size contiguous pop slots, advances tail immediately,
	// and returns pointer + count. Read from ptr after the call — no commit needed.
	RB_OPT constexpr ConstContiguousArea reserve_pop(size_t max_size) {
		Guard g;
		uint32_t s          = static_cast<uint32_t>(state); // single LDR
		uint16_t h          = getHead(s);
		uint16_t t          = getTail(s);
		size_t   available  = wrapSize((uint32_t)h - (uint32_t)t + Size);
		size_t   contiguous = Size - t;
		if (contiguous > available) contiguous = available;
		if (contiguous > max_size)  contiguous = max_size;
		const element_type* ptr = &buffer[t];
		if constexpr (is_power_of_two)
			t = static_cast<uint16_t>((t + contiguous) & (Size - 1));
		else
			t = static_cast<uint16_t>((t + contiguous) % Size);
		state = pack(h, t);                                  // single STR
		return { ptr, contiguous };
	}
};

#undef RB_OPT
#undef RB_OPT_INLINE
