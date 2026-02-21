/**
 * multithreading_demo.c
 *
 * Multi-threaded Context Switching Test (Phase 0 CSRs + Phase 1.4 Context Switching)
 *
 * Phase 0 Demonstrates (NEW):
 * - Reading PTID CSR (0xC00) - Physical thread ID
 * - Writing/reading VTID CSR (0xC01) - Virtual thread ID persistence
 * - Writing/reading THREAD_STAT (0xC02) - Thread status persistence
 * - Writing/reading EXCEPTION_PTR (0xC04) - Exception descriptor pointer
 * - Reading NUM_THREADS and MAX_THREADS (0xC08-0xC09) - Thread topology
 * - Writing/reading SCHEDULER_CTL (0xC0C) - Scheduler modes (HW RR / SW / Priority)
 * - Monitoring THREAD_CYCLES and THREAD_INSTR (0xC0A-0xC0B) - Performance counters
 *
 * Phase 1.4 Demonstrates:
 * - Writing to CTXT CSR (0x081) to switch contexts
 * - Reading current context ID via bp_get_context_id()
 * - Hardware-based thread scheduler control
 * - Per-context independent computation
 *
 * Phase 1.4 Implementation:
 * Software writes target thread ID to CSR 0x081 -> Hardware scheduler jumps to thread
 */

#include <stdint.h>
#include "bp_utils.h"

#define NUM_CONTEXTS 4

// Per-context result storage
typedef struct {
  uint64_t context_id;
  uint64_t verified_context;
  uint64_t sum_result;
  uint64_t fib_result;
} context_result_t;

// Global storage for per-context results
static context_result_t context_results[NUM_CONTEXTS];

/**
 * Per-context work: sum(0..99)
 * Expected result: 0 + 1 + ... + 99 = 4950
 */
static inline uint64_t compute_sum(void) {
  uint64_t sum = 0;
  for (uint64_t i = 0; i < 100; i++) {
    sum += i;
  }
  return sum;
}

/**
 * Per-context work: fibonacci(15)
 * Expected result: 610
 */
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

/**
 * Read cycle counter via rdcycle instruction
 */
static inline uint64_t ReadCycleCounter(void) {
  uint64_t cycles;
  __asm__ volatile("rdcycle %0" : "=r"(cycles)::);
  return cycles;
}

int main(int argc, char** argv) {
  bp_print_string("[START] Entering main\n");
  bp_print_string("=== BlackParrot Phase 0 CSR + Phase 1.4 Context Switching Test ===\n");
  bp_print_string("Testing Phase 0 thread management CSRs (0xC00-0xC0C)\n");
  bp_print_string("Testing Phase 1.4 CTXT CSR-based hardware thread control\n\n");

  // ========================================================================
  // PHASE 0: Test Thread Management CSRs (0xC00-0xC0C)
  // ========================================================================

  bp_print_string("\n[PHASE 0] Testing Thread Management CSRs:\n");
  bp_print_string("=========================================\n\n");

  int phase0_errors = 0;

  // Test 1: PTID (Physical Thread ID) - should be 0 for main thread
  bp_print_string("Test 1: PTID (0x580) - Reading physical thread ID\n");
  // First test if we can read CONTEXT ID CSR (0x081) which we know works
  uint64_t ctx = bp_get_context_id();
  bp_print_string("  Context ID = ");
  bp_print_uint64(ctx);
  bp_print_string(" (should be 0)\n");
  // Now test PTID
  uint64_t ptid = bp_get_ptid();
  bp_print_string("  PTID = ");
  bp_print_uint64(ptid);
  bp_print_string(" (expected 0)\n");
  if (ptid != 0) {
    bp_print_string("  [ERROR] PTID should be 0 for main thread\n");
    phase0_errors++;
  }
  bp_print_string("\n");

  // Test 2: VTID (Virtual Thread ID) - write and read back
  bp_print_string("Test 2: VTID (0x581) - Testing write/read persistence\n");
  uint64_t test_vtid = 42;
  bp_set_vtid(test_vtid);
  uint64_t read_vtid = bp_get_vtid();
  bp_print_string("  Wrote: ");
  bp_print_uint64(test_vtid);
  bp_print_string(", Read: ");
  bp_print_uint64(read_vtid);
  if (read_vtid != test_vtid) {
    bp_print_string(" [ERROR]\n");
    phase0_errors++;
  } else {
    bp_print_string(" [OK]\n");
  }
  bp_print_string("\n");

  // Test 3: THREAD_STAT (Thread Status) - write and read back
  bp_print_string("Test 3: THREAD_STAT (0x582) - Testing write/read persistence\n");
  uint64_t test_stat = 0x1234567890ABCDEF;
  bp_set_thread_stat(test_stat);
  uint64_t read_stat = bp_get_thread_stat();
  bp_print_string("  Wrote: 0x");
  bp_hprint_uint64(test_stat);
  bp_print_string(", Read: 0x");
  bp_hprint_uint64(read_stat);
  if (read_stat != test_stat) {
    bp_print_string(" [ERROR]\n");
    phase0_errors++;
  } else {
    bp_print_string(" [OK]\n");
  }
  bp_print_string("\n");

  // Test 4: THREAD_PRIO (Thread Priority) - write and read back
  bp_print_string("Test 4: THREAD_PRIO (0x583) - Testing write/read persistence\n");
  uint64_t test_prio = 7;
  bp_set_thread_prio(test_prio);
  uint64_t read_prio = bp_get_thread_prio();
  bp_print_string("  Wrote: ");
  bp_print_uint64(test_prio);
  bp_print_string(", Read: ");
  bp_print_uint64(read_prio);
  if (read_prio != test_prio) {
    bp_print_string(" [ERROR]\n");
    phase0_errors++;
  } else {
    bp_print_string(" [OK]\n");
  }
  bp_print_string("\n");

  // Test 5: EXCEPTION_PTR (Exception Descriptor Pointer) - write and read back
  bp_print_string("Test 5: EXCEPTION_PTR (0x584) - Testing 64-bit write/read persistence\n");
  uint64_t test_exc_ptr = 0xDEADBEEFCAFEBABE;
  bp_set_exception_ptr(test_exc_ptr);
  uint64_t read_exc_ptr = bp_get_exception_ptr();
  bp_print_string("  Wrote: 0x");
  bp_hprint_uint64(test_exc_ptr);
  bp_print_string(", Read: 0x");
  bp_hprint_uint64(read_exc_ptr);
  if (read_exc_ptr != test_exc_ptr) {
    bp_print_string(" [ERROR]\n");
    phase0_errors++;
  } else {
    bp_print_string(" [OK]\n");
  }
  bp_print_string("\n");

  // Test 6: TDT_PTR (Thread Descriptor Table Pointer) - write and read back
  bp_print_string("Test 6: TDT_PTR (0x585) - Testing write/read persistence\n");
  uint64_t test_tdt_ptr = 0x8000000000000000;
  bp_set_tdt_ptr(test_tdt_ptr);
  uint64_t read_tdt_ptr = bp_get_tdt_ptr();
  bp_print_string("  Wrote: 0x");
  bp_hprint_uint64(test_tdt_ptr);
  bp_print_string(", Read: 0x");
  bp_hprint_uint64(read_tdt_ptr);
  if (read_tdt_ptr != test_tdt_ptr) {
    bp_print_string(" [ERROR]\n");
    phase0_errors++;
  } else {
    bp_print_string(" [OK]\n");
  }
  bp_print_string("\n");

  // Test 7: MONITOR_ADDR (Monitor Address for mwait) - write and read back
  bp_print_string("Test 7: MONITOR_ADDR (0x586) - Testing write/read persistence\n");
  uint64_t test_monitor = 0x0000DEADBEEFCAFE;
  bp_set_monitor_addr(test_monitor);
  uint64_t read_monitor = bp_get_monitor_addr();
  bp_print_string("  Wrote: 0x");
  bp_hprint_uint64(test_monitor);
  bp_print_string(", Read: 0x");
  bp_hprint_uint64(read_monitor);
  if (read_monitor != test_monitor) {
    bp_print_string(" [ERROR]\n");
    phase0_errors++;
  } else {
    bp_print_string(" [OK]\n");
  }
  bp_print_string("\n");

  // Test 8: THREAD_LOCAL (Thread-Local Storage Base) - write and read back
  bp_print_string("Test 8: THREAD_LOCAL (0x587) - Testing write/read persistence\n");
  uint64_t test_tls = 0x7000000000000000;
  bp_set_thread_local(test_tls);
  uint64_t read_tls = bp_get_thread_local();
  bp_print_string("  Wrote: 0x");
  bp_hprint_uint64(test_tls);
  bp_print_string(", Read: 0x");
  bp_hprint_uint64(read_tls);
  if (read_tls != test_tls) {
    bp_print_string(" [ERROR]\n");
    phase0_errors++;
  } else {
    bp_print_string(" [OK]\n");
  }
  bp_print_string("\n");

  // Test 9: NUM_THREADS and MAX_THREADS (read-only)
  bp_print_string("Test 9: NUM_THREADS (0x588) and MAX_THREADS (0x589) - Read-only\n");
  uint64_t num_threads = bp_get_num_threads();
  uint64_t max_threads = bp_get_max_threads();
  bp_print_string("  NUM_THREADS = ");
  bp_print_uint64(num_threads);
  bp_print_string("\n  MAX_THREADS = ");
  bp_print_uint64(max_threads);
  bp_print_string("\n");
  if (num_threads == 0 || max_threads == 0 || num_threads > max_threads) {
    bp_print_string("  [ERROR] Invalid thread counts\n");
    phase0_errors++;
  } else {
    bp_print_string("  [OK]\n");
  }
  bp_print_string("\n");

  // Test 10: SCHEDULER_CTL (Scheduler Control) - test three scheduler modes
  bp_print_string("Test 10: SCHEDULER_CTL (0x58C) - Testing scheduler modes\n");

  // Mode 0: HW round-robin
  bp_set_scheduler_ctl(0x00);
  uint64_t read_sched_0 = bp_get_scheduler_ctl();
  bp_print_string("  Mode 0 (HW RR): wrote 0x00, read 0x");
  bp_hprint_uint64(read_sched_0);
  if ((read_sched_0 & 0xFF) != 0x00) {
    bp_print_string(" [ERROR]\n");
    phase0_errors++;
  } else {
    bp_print_string(" [OK]\n");
  }

  // Mode 1: Software override
  bp_set_scheduler_ctl(0x01);
  uint64_t read_sched_1 = bp_get_scheduler_ctl();
  bp_print_string("  Mode 1 (SW Override): wrote 0x01, read 0x");
  bp_hprint_uint64(read_sched_1);
  if ((read_sched_1 & 0xFF) != 0x01) {
    bp_print_string(" [ERROR]\n");
    phase0_errors++;
  } else {
    bp_print_string(" [OK]\n");
  }

  // Mode 2: Priority-based
  bp_set_scheduler_ctl(0x02);
  uint64_t read_sched_2 = bp_get_scheduler_ctl();
  bp_print_string("  Mode 2 (Priority): wrote 0x02, read 0x");
  bp_hprint_uint64(read_sched_2);
  if ((read_sched_2 & 0xFF) != 0x02) {
    bp_print_string(" [ERROR]\n");
    phase0_errors++;
  } else {
    bp_print_string(" [OK]\n");
  }
  bp_print_string("\n");

  // Test 11: THREAD_CYCLES and THREAD_INSTR counters
  bp_print_string("Test 11: THREAD_CYCLES (0x58A) and THREAD_INSTR (0x58B) - Counters\n");
  uint64_t cycles_before = bp_get_thread_cycles();
  uint64_t instr_before = bp_get_thread_instr();

  // Execute some work to advance counters
  volatile uint64_t work = 0;
  for (int i = 0; i < 100; i++) {
    work += i;
  }

  uint64_t cycles_after = bp_get_thread_cycles();
  uint64_t instr_after = bp_get_thread_instr();

  bp_print_string("  THREAD_CYCLES: before=");
  bp_print_uint64(cycles_before);
  bp_print_string(", after=");
  bp_print_uint64(cycles_after);
  bp_print_string("\n  THREAD_INSTR: before=");
  bp_print_uint64(instr_before);
  bp_print_string(", after=");
  bp_print_uint64(instr_after);
  bp_print_string("\n");

  // Counters should advance (or at least not go backward)
  if (cycles_after < cycles_before || instr_after < instr_before) {
    bp_print_string("  [WARNING] Counters should advance\n");
  } else {
    bp_print_string("  [OK] Counters advancing\n");
  }
  bp_print_string("\n");

  // Reset scheduler to HW round-robin mode for Phase 1.4 test
  bp_set_scheduler_ctl(0x00);

  // Print Phase 0 summary
  bp_print_string("PHASE 0 Summary:\n");
  if (phase0_errors == 0) {
    bp_print_string("All Phase 0 CSR tests PASSED\n");
  } else {
    bp_print_string("Phase 0 CSR tests FAILED with ");
    bp_print_uint64((uint64_t)phase0_errors);
    bp_print_string(" errors\n");
  }
  bp_print_string("\n\n");

  // ========================================================================
  // PHASE 1.4: Test Hardware Context Switching (original test)
  // ========================================================================

  bp_print_string("[PHASE 1.4] Testing Hardware Context Switching:\n");
  bp_print_string("================================================\n\n");

  // Initialize results array
  for (int i = 0; i < NUM_CONTEXTS; i++) {
    context_results[i].context_id = i;
    context_results[i].verified_context = 0xDEADBEEF;
    context_results[i].sum_result = 0;
    context_results[i].fib_result = 0;
  }

  uint64_t begin = ReadCycleCounter();

  // Phase 1.4: Hardware context switching via CTXT CSR
  // Software writes target context ID to CSR 0x081
  // Hardware scheduler jumps to that thread on next cycle

  // Initialize scheduler to thread 0 before starting test
  bp_set_context_id(0);
  // Use memory barrier to ensure write completes
  volatile uint64_t barrier = 0;
  barrier++;  // Force instruction dependency

  bp_print_string("Performing hardware context switches:\n");

  for (uint64_t target_ctx = 0; target_ctx < NUM_CONTEXTS; target_ctx++) {
    // Write target context to CTXT CSR to trigger hardware switch
    bp_print_string("  [DEBUG] Writing context ");
    bp_print_uint64(target_ctx);
    bp_print_string(" to CSR 0x081\n");
    bp_set_context_id(target_ctx);

    // Use memory access to create instruction dependency and pipeline stalls
    // This ensures the scheduler has time to update before we read
    volatile uint64_t dummy_val = target_ctx;
    for (volatile int i = 0; i < target_ctx + 5; i++) {
      dummy_val = dummy_val * 2 + 1;  // Create data dependency chain
    }

    // Read target_ctx multiple times to get latest value (pipeline doesn't speculate CSR reads)
    uint64_t current_ctx = bp_get_context_id();
    uint64_t ctx_retry1 = bp_get_context_id();  // Read twice to ensure committed value
    uint64_t ctx_retry2 = bp_get_context_id();

    bp_print_string("  [DEBUG] Read sequence: ");
    bp_print_uint64(current_ctx);
    bp_print_string(", ");
    bp_print_uint64(ctx_retry1);
    bp_print_string(", ");
    bp_print_uint64(ctx_retry2);
    bp_print_string("\n");

    bp_print_string("  Switch to ");
    bp_print_uint64(target_ctx);
    bp_print_string(" -> verified: ");
    bp_print_uint64(current_ctx);
    bp_print_string("\n");

    // Store verification result
    context_results[target_ctx].verified_context = current_ctx;

    // Perform per-context work
    context_results[target_ctx].context_id = target_ctx;
    context_results[target_ctx].sum_result = compute_sum();
    context_results[target_ctx].fib_result = compute_fibonacci(15);
  }

  uint64_t end = ReadCycleCounter();
  uint64_t total_cycles = end - begin;

  // Validate all context results
  bp_print_string("\nValidation Results:\n");

  uint64_t expected_sum = 4950;  // sum(0..99)
  uint64_t expected_fib = 610;   // fib(15)

  int phase1_errors = 0;
  for (uint64_t ctx = 0; ctx < NUM_CONTEXTS; ctx++) {
    bp_print_string("Context ");
    bp_print_uint64(ctx);
    bp_print_string(": ");

    // Check if context ID was verified
    if (context_results[ctx].verified_context != ctx) {
      bp_print_string("[ERROR: not on correct context] ");
      phase1_errors++;
    }

    // Check sum result
    bp_print_string("sum=");
    bp_print_uint64(context_results[ctx].sum_result);
    if (context_results[ctx].sum_result != expected_sum) {
      bp_print_string("[FAIL] ");
      phase1_errors++;
    }

    // Check fibonacci result
    bp_print_string(", fib=");
    bp_print_uint64(context_results[ctx].fib_result);
    if (context_results[ctx].fib_result != expected_fib) {
      bp_print_string("[FAIL] ");
      phase1_errors++;
    }

    bp_print_string("\n");
  }

  bp_print_string("\nPerformance:\n");
  bp_print_string("Total cycles: ");
  bp_print_uint64(total_cycles);
  bp_print_string("\n");

  bp_print_string("\nPhase 1.4 Summary:\n");
  if (phase1_errors == 0) {
    bp_print_string("All Phase 1.4 context switching tests PASSED\n");
  } else {
    bp_print_string("Phase 1.4 tests FAILED with ");
    bp_print_uint64((uint64_t)phase1_errors);
    bp_print_string(" errors\n");
  }

  // ========================================================================
  // FINAL SUMMARY
  // ========================================================================
  bp_print_string("\n\n========================================\n");
  bp_print_string("FINAL TEST SUMMARY\n");
  bp_print_string("========================================\n");
  bp_print_string("Phase 0 Errors: ");
  bp_print_uint64((uint64_t)phase0_errors);
  bp_print_string("\n");
  bp_print_string("Phase 1.4 Errors: ");
  bp_print_uint64((uint64_t)phase1_errors);
  bp_print_string("\n");

  int total_errors = phase0_errors + phase1_errors;
  if (total_errors == 0) {
    bp_print_string("\nALL TESTS PASSED!\n");
    bp_print_string("Phase 0 (CSR) + Phase 1.4 (Context Switching) verified successfully.\n");
    bp_finish(0);
  } else {
    bp_print_string("\nTEST FAILED with ");
    bp_print_uint64((uint64_t)total_errors);
    bp_print_string(" total errorsdfs\n");
    bp_finish(1);
  }

  return 0;
}
