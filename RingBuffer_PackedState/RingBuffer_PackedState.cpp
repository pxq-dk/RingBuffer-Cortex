/*
 * RingBuffer_PackedState.cpp
 *
 *  Created on: 18. maj 2026
 *      Author: Erik Nørskov
 *
 *  Copyright (c) 2026 Erik Nørskov / PxQ Technologies
 *  https://pxq.dk
 *
 *  Dual License:
 *
 *  1. GNU General Public License v3.0 (GPLv3)
 *     This file is free software: you can redistribute it and/or modify
 *     it under the terms of the GNU General Public License as published by
 *     the Free Software Foundation, version 3 of the License.
 *
 *     This file is distributed in the hope that it will be useful,
 *     but WITHOUT ANY WARRANTY; without even the implied warranty of
 *     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 *     General Public License for more details: https://www.gnu.org/licenses/
 *
 *  2. Commercial License
 *     For use in proprietary or closed-source products that cannot or
 *     do not wish to comply with the GPLv3, a separate commercial license
 *     is available from PxQ Technologies — either as a written agreement,
 *     or via direct delivery by Erik Nørskov as part of a paid engagement
 *     (in which case the license is granted for that specific project scope only).
 *
 *     Each commercial license covers only the version of the software
 *     actually delivered into the licensee's project by the licensor.
 *     Later versions become covered only when likewise delivered as
 *     part of a paid engagement or written agreement, or when the
 *     licensee obtains a separate paid license for that later version.
 *     The licensee may not substitute or upgrade the software to any
 *     later version on their own initiative without such a license.
 *
 *     Contact: https://pxq.dk
 */

/*
 *  Runtime self-test for RingBuffer_PackedState.
 *
 *  The compile-time test suite (unit_test_ringbuffer::run_test) verifies
 *  logical correctness via static_assert. This runtime entry point reuses
 *  the same suite but executes against the real machine code — catching
 *  what the static_assert path cannot: volatile-qualified accesses, the
 *  32-bit LDR snapshot in readHT(), memcpy paths in push_n/pop_n, and -Os
 *  codegen quirks.
 *
 *  First pass: SPSC, uint8_t. Limited to 3 representative sizes (8 / 32 / 512)
 *  for now — instantiating all 8 power-of-two sizes overflows flash on the
 *  G051. The full set returns once the force_minimum_code_bloat split-impl
 *  refactor lands and all sizes share one implementation copy.
 */
#include "stm32g0xx_hal.h"
#include "RingBuffer_PackedState.h"

#if RB_ENABLE_RUNTIME_TESTS


bool RingBuffer_PackedState_runtime_selftest()
{
	bool ok = true;
	ok &= unit_test_ringbuffer<RingBuffer_PackedState<uint8_t,   8, Topology::SPSC<>>>::run_test();
	ok &= unit_test_ringbuffer<RingBuffer_PackedState<uint8_t,  32, Topology::SPSC<>>>::run_test();   // production size
	ok &= unit_test_ringbuffer<RingBuffer_PackedState<uint8_t, 512, Topology::SPSC<>>>::run_test();
	return ok;
}


/*
 * Layer 2: concurrent SPSC stress test.
 *   Producer (timer ISR) pushes a monotonic uint8_t counter.
 *   Consumer (main loop) pops and verifies each value equals expected++.
 *   On the first sequence mismatch, BKPT halts for SWD inspection — the
 *   debugger context is most useful immediately, before any further pop.
 *   Drops are expected (consumer outpaces producer in steady state) and
 *   counted but not an error.
 */

static RingBuffer_PackedState<uint8_t, 16, Topology::SPSC<>> stress_buffer;
// Not volatile: each is accessed from a single context only — producer_seq
// from the producer ISR, expected from the main-loop consumer. No cross-
// context sharing, so no volatile needed. Marking them volatile would also
// break push(const T&) which cannot bind to a volatile lvalue.
static uint8_t stress_producer_seq = 0;
static uint8_t stress_expected     = 0;


volatile uint32_t RingBuffer_PackedState_stress_pushes     = 0;
volatile uint32_t RingBuffer_PackedState_stress_drops      = 0;
volatile uint32_t RingBuffer_PackedState_stress_pops       = 0;
volatile uint32_t RingBuffer_PackedState_stress_seq_errors = 0;

void RingBuffer_PackedState_stress_init()
{
	stress_buffer.clear();
	stress_producer_seq = 0;
	stress_expected     = 0;
	RingBuffer_PackedState_stress_pushes     = 0;
	RingBuffer_PackedState_stress_drops      = 0;
	RingBuffer_PackedState_stress_pops       = 0;
	RingBuffer_PackedState_stress_seq_errors = 0;
}

void RingBuffer_PackedState_stress_producer_tick()
{
	if (stress_buffer.push(stress_producer_seq)) {
		++RingBuffer_PackedState_stress_pushes;
	} else {
		++RingBuffer_PackedState_stress_drops;
	}
	++stress_producer_seq;
}

void RingBuffer_PackedState_stress_consumer_tick()
{
	// Hold off draining until the buffer is at least half full (16/32).
	// The subsequent tight pop-loop then runs long enough that producer ISR
	// firings have a real chance of overlapping with the consumer's
	// readHT() — that's what actually stresses the atomic-snapshot path
	// under concurrent access. Pure drain-on-every-tick keeps the buffer
	// near-empty and the race window near-zero.
	if (stress_buffer.getCount() < (stress_buffer.capacity+1)/2) return;

	uint8_t v;
	while (stress_buffer.pop(v)) {
		if (v != stress_expected) {
			++RingBuffer_PackedState_stress_seq_errors;
			__BKPT(0);
		}
		++stress_expected;
		++RingBuffer_PackedState_stress_pops;
	}
}


// Bare-metal TIM6 IRQ handler. Bypasses HAL_TIM_IRQHandler so the library
// owns the entire IRQ path — no dispatch via HAL_TIM_PeriodElapsedCallback,
// which means the project's existing strong implementation of that callback
// stays untouched.
extern "C" void TIM6_DAC_LPTIM1_IRQHandler(void)
{
	if (TIM6->SR & TIM_SR_UIF) {
		TIM6->SR = (uint32_t)~TIM_SR_UIF;       // clear the update flag
		RingBuffer_PackedState_stress_producer_tick();
	}
}


// One-call init: Layer 1 selftest (BKPT on fail) + Layer 2 stress arm.
// Caller still needs to drive RingBuffer_PackedState_stress_consumer_tick()
// from the main loop. Precondition: MX_TIM6_Init() has run.
void RingBuffer_PackedState_runtime_tests_init()
{
	// Layer 1
	if (!RingBuffer_PackedState_runtime_selftest()) {
		__BKPT(0);
	}

	// Layer 2 — state
	RingBuffer_PackedState_stress_init();

	// Layer 2 — TIM6 timing override (64 MHz / (PSC+1) / (ARR+1)).
	// PSC=0, ARR=639 → 100 kHz producer rate.
	TIM6->PSC   = 0;
	TIM6->ARR   = 639;
	TIM6->EGR   = TIM_EGR_UG;        // load new PSC/ARR into shadow
	TIM6->SR    = 0;                 // clear pending IRQ flags (UG just set one)
	TIM6->DIER |= TIM_DIER_UIE;      // enable update interrupt
	TIM6->CR1  |= TIM_CR1_CEN;       // start counter

	// Layer 2 — NVIC
	HAL_NVIC_SetPriority(TIM6_DAC_LPTIM1_IRQn, 2, 0);
	HAL_NVIC_EnableIRQ(TIM6_DAC_LPTIM1_IRQn);
}

#endif // RB_ENABLE_RUNTIME_TESTS
