/**
 * mt_frf_isolation_test.c
 *
 * FP Register File Isolation Test
 *
 * Verifies that each hardware thread has its own independent FP register state
 * via bp_be_regfile_mt (which is used for both integer and FP register files).
 *
 * Test sequence:
 *   1. T0: write SENTINEL_T0 to f1 via fmv.d.x
 *   2. T0: read back f1 and verify
 *   3. Launch T1
 *   4. T1: read f1 — must be 0 (clean per-thread state; no T0 spillover)
 *   5. T1: write SENTINEL_T1 to f1
 *   6. T1: read back f1 and verify write took effect
 *   7. T1: switch back to T0 (csrw 0x081, 0)
 *   8. T0: read f1 — must still be SENTINEL_T0 (not corrupted by T1)
 *
 * Before FP regfile isolation: T0 would see SENTINEL_T1 → FAIL
 * After  FP regfile isolation: T0 sees SENTINEL_T0          → PASS
 */

#include <stdint.h>
#include "bp_utils.h"

#define SENTINEL_T0 0xDEADBEEF00000000ULL
#define SENTINEL_T1 0xBEEFDEAD00000000ULL

#define STACK_WORDS 512
static uint64_t t1_stack[STACK_WORDS];

/* Shared result area written by T1 so T0 can verify */
static volatile uint64_t t1_f1_initial = 0xFFFFFFFFFFFFFFFFULL;
static volatile uint64_t t1_f1_final   = 0xFFFFFFFFFFFFFFFFULL;

/* ── FP register helpers (f1) ── */
static inline void write_f1(uint64_t val) {
  __asm__ volatile("fmv.d.x f1, %0" : : "r"(val) : "f1");
}

static inline uint64_t read_f1(void) {
  uint64_t val;
  __asm__ volatile("fmv.x.d %0, f1" : "=r"(val));
  return val;
}

/* ── CSR helpers ── */
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

/* ── Thread 1 entry ── */
void __attribute__((noinline)) t1_entry(void) {
  /* T1 has a fresh mstatus (FS=Off at reset) — enable FP before any FP instruction.
   * mstatus.FS bits [14:13]: 00=Off, 01=Initial, 10=Clean, 11=Dirty.
   * csrs sets bits without clobbering other mstatus fields. */
  __asm__ volatile("csrs mstatus, %0" : : "r"(3ULL << 13));

  /* T1 reads f1 — should be 0 (own clean state, not T0's sentinel) */
  t1_f1_initial = read_f1();

  /* T1 writes its own sentinel to f1 */
  write_f1(SENTINEL_T1);

  /* Confirm write took effect in T1's own regfile */
  t1_f1_final = read_f1();

  /* Return to T0 */
  write_ctxt(0);
  bp_finish(1);  /* unreachable */
}

int main(void) {
  bp_print_string("=== FP Register File Isolation Test ===\n");

  /* ── Step 1: T0 writes sentinel to f1 ── */
  write_f1(SENTINEL_T0);
  uint64_t t0_f1_initial = read_f1();

  bp_print_string("T0 f1 initial: ");
  bp_hprint_uint64(t0_f1_initial);
  bp_print_string("\n");

  if (t0_f1_initial != SENTINEL_T0) {
    bp_print_string("FAIL: T0 f1 write/read mismatch\n");
    bp_finish(1);
  }

  /* ── Step 2: Seed and launch T1 ── */
  uint64_t gp_val;
  __asm__ volatile("mv %0, gp" : "=r"(gp_val));
  seed_reg(1, 3 /* x3=gp */, gp_val);
  seed_reg(1, 2 /* x2=sp */, (uint64_t)&t1_stack[STACK_WORDS]);
  seed_npc(1, (uint64_t)t1_entry);
  write_ctxt(1);
  /* T0 resumes here after T1 calls write_ctxt(0) */

  /* ── Step 3: Verify T0's f1 is unchanged ── */
  uint64_t t0_f1_after = read_f1();

  bp_print_string("T0 f1 after T1: ");
  bp_hprint_uint64(t0_f1_after);
  bp_print_string("\n");

  bp_print_string("T1 f1 initial:  ");
  bp_hprint_uint64(t1_f1_initial);
  bp_print_string("\n");

  bp_print_string("T1 f1 final:    ");
  bp_hprint_uint64(t1_f1_final);
  bp_print_string("\n");

  int errors = 0;

  /* T0's f1 must still be SENTINEL_T0 */
  if (t0_f1_after != SENTINEL_T0) {
    bp_print_string("FAIL: T0 f1 was corrupted by T1\n");
    bp_print_string("  expected: ");
    bp_hprint_uint64(SENTINEL_T0);
    bp_print_string("\n  got:      ");
    bp_hprint_uint64(t0_f1_after);
    bp_print_string("\n");
    errors++;
  } else {
    bp_print_string("PASS: T0 f1 preserved across T1 execution\n");
  }

  /* T1 should have started with f1 = 0 (per-thread clean state) */
  if (t1_f1_initial != 0) {
    bp_print_string("FAIL: T1 f1 not 0 at entry (leaked from T0?)\n");
    bp_print_string("  got: ");
    bp_hprint_uint64(t1_f1_initial);
    bp_print_string("\n");
    errors++;
  } else {
    bp_print_string("PASS: T1 f1 was 0 at entry (clean isolation)\n");
  }

  /* T1 should have written its sentinel successfully */
  if (t1_f1_final != SENTINEL_T1) {
    bp_print_string("FAIL: T1 f1 write did not take effect\n");
    bp_print_string("  expected: ");
    bp_hprint_uint64(SENTINEL_T1);
    bp_print_string("\n  got:      ");
    bp_hprint_uint64(t1_f1_final);
    bp_print_string("\n");
    errors++;
  } else {
    bp_print_string("PASS: T1 f1 write succeeded\n");
  }

  bp_print_string("\n");
  if (errors == 0) {
    bp_print_string("[BSG-PASS] FP regfile isolation verified\n");
    bp_finish(0);
  } else {
    bp_print_string("[BSG-FAIL] FP regfile isolation test failed\n");
    bp_finish(1);
  }

  return 0;
}
