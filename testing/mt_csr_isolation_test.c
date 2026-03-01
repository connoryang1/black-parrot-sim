/**
 * mt_csr_isolation_test.c
 *
 * Phase 2A: CSR Isolation Test
 *
 * Verifies that each hardware thread has its own independent CSR state.
 * Uses mscratch (0x340) as the sentinel register — a plain R/W CSR with
 * no special accumulation or auto-update behavior.
 *
 * Test sequence:
 *   1. Thread 0: write SENTINEL_T0 to mscratch
 *   2. Switch to thread 1; thread 1 writes SENTINEL_T1 to mscratch
 *   3. Thread 1 switches back to thread 0
 *   4. Thread 0 reads mscratch — must be SENTINEL_T0 (not SENTINEL_T1)
 *
 * Before Phase 2A (shared CSR file): T0 would see SENTINEL_T1 → FAIL
 * After  Phase 2A (per-thread CSRs): T0 sees SENTINEL_T0          → PASS
 */

#include <stdint.h>
#include "bp_utils.h"

/* Sentinels: clearly distinct values */
#define SENTINEL_T0 0xA5A5A5A5ULL
#define SENTINEL_T1 0x5A5A5A5AULL

/* Per-thread stack for thread 1 (4 KB) */
#define STACK_WORDS 512
static uint64_t t1_stack[STACK_WORDS];

/* Shared result area written by thread 1 so thread 0 can check it */
static volatile uint64_t t1_initial_mscratch = 0xFFFFFFFFFFFFFFFFULL;
static volatile uint64_t t1_final_mscratch   = 0xFFFFFFFFFFFFFFFFULL;

/* ── CSR helpers ── */
static inline void write_ctxt(uint64_t v) {
  __asm__ volatile("csrw 0x081, %0" : : "r"(v));
}

static inline void seed_npc(uint64_t tid, uint64_t npc) {
  /* bits [38:0] = NPC, bits [40:39] = thread_id (vaddr_width_p=39) */
  uint64_t v = ((tid & 0x3ULL) << 39) | (npc & 0x7FFFFFFFFFULL);
  __asm__ volatile("csrw 0x082, %0" : : "r"(v));
}

static inline void seed_reg(uint64_t tid, uint64_t reg, uint64_t val) {
  /* bits [38:0]=val, bits[40:39]=tid, bits[45:41]=reg_addr */
  uint64_t v = (val & 0x7FFFFFFFFFULL)
             | ((tid & 0x3ULL) << 39)
             | ((reg & 0x1FULL) << 41);
  __asm__ volatile("csrw 0x083, %0" : : "r"(v));
}

static inline uint64_t read_mscratch(void) {
  uint64_t v;
  __asm__ volatile("csrr %0, mscratch" : "=r"(v));
  return v;
}

static inline void write_mscratch(uint64_t v) {
  __asm__ volatile("csrw mscratch, %0" : : "r"(v));
}

/* ── Thread 1 entry ── */
void __attribute__((noinline)) t1_entry(void) {
  /* After Phase 2A: T1 starts with its own CSR file, mscratch = 0 (reset) */
  t1_initial_mscratch = read_mscratch();

  /* Write T1's sentinel */
  write_mscratch(SENTINEL_T1);

  /* Confirm T1's write took effect in its own file */
  t1_final_mscratch = read_mscratch();

  /* Return to thread 0 */
  write_ctxt(0);
  bp_finish(1);  /* unreachable */
}

int main(void) {
  bp_print_string("=== Phase 2A: CSR Isolation Test ===\n");

  /* ── Step 1: T0 writes sentinel ── */
  write_mscratch(SENTINEL_T0);
  uint64_t t0_initial = read_mscratch();

  bp_print_string("T0 mscratch initial: ");
  bp_hprint_uint64(t0_initial);
  bp_print_string("\n");

  if (t0_initial != SENTINEL_T0) {
    bp_print_string("FAIL: T0 mscratch write/read mismatch\n");
    bp_finish(1);
  }

  /* ── Step 2: Seed and launch thread 1 ── */
  /* Thread 1 shares the same binary/address space, so it needs the same GP */
  uint64_t gp_val;
  __asm__ volatile("mv %0, gp" : "=r"(gp_val));
  seed_reg(1, 3 /* x3=gp */, gp_val);
  seed_reg(1, 2 /* x2=sp */, (uint64_t)&t1_stack[STACK_WORDS]);
  seed_npc(1, (uint64_t)t1_entry);
  write_ctxt(1);
  /* T0 resumes here after T1 calls write_ctxt(0) */

  /* ── Step 3: Verify T0's mscratch is unchanged ── */
  uint64_t t0_after = read_mscratch();

  bp_print_string("T0 mscratch after T1: ");
  bp_hprint_uint64(t0_after);
  bp_print_string("\n");

  bp_print_string("T1 mscratch initial:  ");
  bp_hprint_uint64(t1_initial_mscratch);
  bp_print_string("\n");

  bp_print_string("T1 mscratch final:    ");
  bp_hprint_uint64(t1_final_mscratch);
  bp_print_string("\n");

  int errors = 0;

  /* T0's mscratch must still be SENTINEL_T0 */
  if (t0_after != SENTINEL_T0) {
    bp_print_string("FAIL: T0 mscratch was corrupted by T1\n");
    bp_print_string("  expected: ");
    bp_hprint_uint64(SENTINEL_T0);
    bp_print_string("\n  got:      ");
    bp_hprint_uint64(t0_after);
    bp_print_string("\n");
    errors++;
  } else {
    bp_print_string("PASS: T0 mscratch preserved across T1 execution\n");
  }

  /* T1 should have started with mscratch = 0 (per-thread reset state) */
  if (t1_initial_mscratch != 0) {
    bp_print_string("FAIL: T1 mscratch not 0 at entry (leaked from T0?)\n");
    errors++;
  } else {
    bp_print_string("PASS: T1 mscratch was 0 at entry (clean isolation)\n");
  }

  /* T1 should have written its sentinel successfully */
  if (t1_final_mscratch != SENTINEL_T1) {
    bp_print_string("FAIL: T1 mscratch write did not take effect\n");
    errors++;
  } else {
    bp_print_string("PASS: T1 mscratch write succeeded\n");
  }

  bp_print_string("\n");
  if (errors == 0) {
    bp_print_string("[BSG-PASS] CSR isolation verified\n");
    bp_finish(0);
  } else {
    bp_print_string("[BSG-FAIL] CSR isolation test failed\n");
    bp_finish(1);
  }

  return 0;
}
