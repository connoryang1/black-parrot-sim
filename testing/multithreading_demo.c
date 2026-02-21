/**
 * multithreading_demo.c
 *
 * Phase 1.4: Hardware Context Switching Test
 *
 * Tests CSR 0x081 (CTXT) - the committed hardware:
 * - Writing a thread ID to CSR 0x081 triggers a hardware context switch
 * - Reading CSR 0x081 returns the current thread ID
 *
 * Phase 0 CSR tests (0x580-0x58C) are NOT included here because the RTL
 * handlers for those CSRs are not yet implemented. Add them when the RTL
 * is ready.
 *
 * No bp_utils wrappers are used for CSR access - all CSR reads/writes are
 * inlined directly so black-parrot-sdk does not need to be modified.
 */

#include <stdint.h>
#include "bp_utils.h"

#define NUM_CONTEXTS 4

/* Inline CSR accessors for CTXT (0x081) - no bp_utils changes needed */
static inline uint64_t read_ctxt(void) {
  uint64_t val;
  __asm__ volatile("csrr %0, 0x081" : "=r"(val) : :);
  return val;
}

static inline void write_ctxt(uint64_t val) {
  __asm__ volatile("csrw 0x081, %0" : : "r"(val) :);
}

/* Per-context result storage */
typedef struct {
  uint64_t context_id;
  uint64_t verified_context;
  uint64_t sum_result;
  uint64_t fib_result;
} context_result_t;

static context_result_t context_results[NUM_CONTEXTS];

/* sum(0..99) = 4950 */
static inline uint64_t compute_sum(void) {
  uint64_t sum = 0;
  for (uint64_t i = 0; i < 100; i++) {
    sum += i;
  }
  return sum;
}

/* fibonacci(15) = 610 */
static inline uint64_t compute_fibonacci(uint64_t n) {
  if (n <= 1) return n;
  uint64_t a = 0, b = 1;
  for (uint64_t i = 2; i <= n; i++) {
    uint64_t tmp = a + b;
    a = b;
    b = tmp;
  }
  return b;
}

static inline uint64_t read_cycle(void) {
  uint64_t cycles;
  __asm__ volatile("rdcycle %0" : "=r"(cycles) : :);
  return cycles;
}

int main(int argc, char** argv) {
  bp_print_string("[START] Entering main\n");
  bp_print_string("=== BlackParrot Phase 1.4 Context Switching Test ===\n");
  bp_print_string("Testing CSR 0x081 (CTXT) hardware thread control\n\n");

  /* Initialize results */
  for (int i = 0; i < NUM_CONTEXTS; i++) {
    context_results[i].context_id = i;
    context_results[i].verified_context = 0xDEADBEEF;
    context_results[i].sum_result = 0;
    context_results[i].fib_result = 0;
  }

  uint64_t begin = read_cycle();

  /* Set context to 0 as a baseline */
  write_ctxt(0);
  /* Memory barrier - force instruction dependency */
  volatile uint64_t barrier = 0;
  barrier++;

  bp_print_string("Performing hardware context switches:\n");

  for (uint64_t target_ctx = 0; target_ctx < NUM_CONTEXTS; target_ctx++) {
    bp_print_string("  [DEBUG] Writing context ");
    bp_hprint_uint64(target_ctx);
    bp_print_string(" to CSR 0x081\n");

    write_ctxt(target_ctx);

    /* Data dependency chain to ensure scheduler has time to update */
    volatile uint64_t dummy = target_ctx;
    for (volatile int i = 0; i < (int)(target_ctx + 5); i++) {
      dummy = dummy * 2 + 1;
    }

    /* Read back current context (read 3 times to get committed value) */
    uint64_t ctx0 = read_ctxt();
    uint64_t ctx1 = read_ctxt();
    uint64_t ctx2 = read_ctxt();

    bp_print_string("  [DEBUG] Read sequence: ");
    bp_hprint_uint64(ctx0);
    bp_print_string(", ");
    bp_hprint_uint64(ctx1);
    bp_print_string(", ");
    bp_hprint_uint64(ctx2);
    bp_print_string("\n");

    bp_print_string("  Switch to ");
    bp_hprint_uint64(target_ctx);
    bp_print_string(" -> verified: ");
    bp_hprint_uint64(ctx0);
    bp_print_string("\n");

    context_results[target_ctx].verified_context = ctx0;
    context_results[target_ctx].context_id = target_ctx;
    context_results[target_ctx].sum_result = compute_sum();
    context_results[target_ctx].fib_result = compute_fibonacci(15);
  }

  uint64_t end = read_cycle();
  uint64_t total_cycles = end - begin;

  /* Validate results */
  bp_print_string("\nValidation Results:\n");

  uint64_t expected_sum = 4950;
  uint64_t expected_fib = 610;

  int errors = 0;
  for (uint64_t ctx = 0; ctx < NUM_CONTEXTS; ctx++) {
    bp_print_string("Context ");
    bp_hprint_uint64(ctx);
    bp_print_string(": ");

    if (context_results[ctx].verified_context != ctx) {
      bp_print_string("[ERROR: wrong context] ");
      errors++;
    }

    bp_print_string("sum=");
    bp_hprint_uint64(context_results[ctx].sum_result);
    if (context_results[ctx].sum_result != expected_sum) {
      bp_print_string("[FAIL] ");
      errors++;
    }

    bp_print_string(", fib=");
    bp_hprint_uint64(context_results[ctx].fib_result);
    if (context_results[ctx].fib_result != expected_fib) {
      bp_print_string("[FAIL] ");
      errors++;
    }

    bp_print_string("\n");
  }

  bp_print_string("\nTotal cycles: ");
  bp_hprint_uint64(total_cycles);
  bp_print_string("\n");

  if (errors == 0) {
    bp_print_string("\nALL TESTS PASSED\n");
    bp_finish(0);
  } else {
    bp_print_string("\nTEST FAILED\n");
    bp_finish(1);
  }

  return 0;
}
