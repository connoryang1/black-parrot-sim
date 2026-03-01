/**
 * mt_benchmark.c
 *
 * Context Switch Latency Benchmark
 *
 * Measures round-trip context switch latency: T0→T1→T0.
 * Thread 1 does nothing except switch back to thread 0 immediately.
 *
 * Expected: ~14 cycles (2 × 7-cycle switches per Banyan target)
 *
 * Target from Banyan paper: 7 cycles per single context switch on an
 * equivalent 8-stage pipeline (see docs/ reference papers).
 *
 * Measurement method:
 *   before = rdcycle           (thread 0)
 *   csrw 0x081, 1              (switch T0→T1)
 *     [thread 1 executes: csrw 0x081, 0 — switch T1→T0]
 *   after = rdcycle            (thread 0 after resuming)
 *   latency = after - before   (round-trip: 2 switches)
 *
 * Runs N_TRIALS back-to-back and reports min/sum.
 */

#include <stdint.h>
#include "bp_utils.h"

#define N_TRIALS 5

#define STACK_WORDS 512
static uint64_t t1_stack[STACK_WORDS];

static inline void write_ctxt(uint64_t v) {
  __asm__ volatile("csrw 0x081, %0" : : "r"(v));
}

static inline void seed_npc(uint64_t tid, uint64_t npc) {
  uint64_t v = ((tid & 0x3ULL) << 39) | (npc & 0x7FFFFFFFFFULL);
  __asm__ volatile("csrw 0x082, %0" : : "r"(v));
}

static inline void seed_reg(uint64_t tid, uint64_t reg, uint64_t val) {
  uint64_t v = (val & 0x7FFFFFFFFFULL)
             | ((tid & 0x3ULL) << 39)
             | ((reg & 0x1FULL) << 41);
  __asm__ volatile("csrw 0x083, %0" : : "r"(v));
}

static inline uint64_t read_cycle(void) {
  uint64_t v;
  __asm__ volatile("rdcycle %0" : "=r"(v));
  return v;
}

/* Thread 1 ping: immediately return to thread 0 */
void __attribute__((noinline)) t1_ping(void) {
  write_ctxt(0);
  bp_finish(1);  /* unreachable */
}

int main(void) {
  bp_print_string("=== Context Switch Latency Benchmark ===\n");
  bp_print_string("Round-trip (T0->T1->T0), thread 1 does nothing\n");
  bp_print_string("Target: ~14 cycles (2 x 7-cycle switches)\n\n");

  /* Seed thread 1 once; same entry point for all trials */
  seed_reg(1, 2 /* sp */, (uint64_t)&t1_stack[STACK_WORDS]);
  seed_npc(1, (uint64_t)t1_ping);

  uint64_t min_cycles = 0xFFFFFFFFFFFFFFFFULL;
  uint64_t total = 0;

  for (int trial = 0; trial < N_TRIALS; trial++) {
    /* Re-seed T1's NPC each trial since T1 has already "returned" */
    seed_npc(1, (uint64_t)t1_ping);

    uint64_t before = read_cycle();
    write_ctxt(1);  /* T0 → T1; T1 immediately does write_ctxt(0) → T0 */
    uint64_t after = read_cycle();

    uint64_t elapsed = after - before;
    total += elapsed;
    if (elapsed < min_cycles)
      min_cycles = elapsed;

    bp_print_string("Trial ");
    bp_hprint_uint64((uint64_t)trial);
    bp_print_string(": ");
    bp_hprint_uint64(elapsed);
    bp_print_string(" cycles\n");
  }

  bp_print_string("\nMin round-trip: ");
  bp_hprint_uint64(min_cycles);
  bp_print_string(" cycles\n");

  bp_print_string("Avg round-trip: ");
  bp_hprint_uint64(total / N_TRIALS);
  bp_print_string(" cycles\n");

  bp_print_string("\nSingle switch estimate (min/2): ");
  bp_hprint_uint64(min_cycles / 2);
  bp_print_string(" cycles\n");

  bp_finish(0);
  return 0;
}
