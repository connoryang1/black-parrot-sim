/**
 * multithreading_demo.c
 *
 * Phase 1.4: Hardware Context Switching Demo
 *
 * Tests CSR 0x081 (CTXT) and CSR 0x082 (CTXT_NPC bootstrap).
 *
 * Each thread is seeded with its own entry point via CSR 0x082, then
 * activated via CSR 0x081. Each thread runs compute_sum() + compute_fibonacci(),
 * stores its results, then switches back to thread 0.
 *
 * Expected results:
 *   sum(0..99)   = 4950  = 0x1356
 *   fibonacci(15) = 610  = 0x262
 */

#include <stdint.h>
#include "bp_utils.h"

#define NUM_CONTEXTS 4

/* Inline CSR accessors */
static inline uint64_t read_ctxt(void) {
  uint64_t val;
  __asm__ volatile("csrr %0, 0x081" : "=r"(val) : :);
  return val;
}

static inline void write_ctxt(uint64_t val) {
  // bp_print_string("[T");
  // bp_hprint_uint64(read_ctxt());
  // bp_print_string("] Writing CTXT = ");
  // bp_hprint_uint64(val);
  // bp_print_string("\n");
  __asm__ volatile("csrw 0x081, %0" : : "r"(val) :);
  // bp_print_string("[T");
  // bp_hprint_uint64(read_ctxt());
  // bp_print_string("] After writing CTXT to ");
  // bp_hprint_uint64(val);
  // bp_print_string(", read back CTXT = ");
  // bp_hprint_uint64(read_ctxt());
  // bp_print_string("\n");
}

/* CSR 0x082: seed a thread's entry NPC.
 * RTL: ctx_npc_write_tid_o = csr_data_li[vaddr_width_p +: thread_id_width_p]
 *      ctx_npc_write_npc_o = csr_data_li[0 +: vaddr_width_p]
 * With vaddr_width_p=39, thread_id_width_p=2:
 *   bits [38:0] = NPC, bits [40:39] = thread_id
 */
static inline void seed_thread_npc(uint64_t thread_id, uint64_t npc) {
  uint64_t val = ((thread_id & 0x3) << 39) | (npc & 0x7FFFFFFFFFULL);

  // bp_print_string("Writing to CSR 0x082: ");
  // bp_hprint_uint64(val);
  // bp_print_string("\n");

  __asm__ volatile("csrw 0x082, %0" : : "r"(val) :);
}

/* CSR 0x083: rpush — write an arbitrary register of a disabled thread.
 * Encoding (vaddr_width_p=39, thread_id_width_p=2, reg_addr_width_gp=5):
 *   bits [38:0]  = value (39-bit)
 *   bits [40:39] = thread_id
 *   bits [45:41] = reg_addr (x0=0, x1=ra, x2=sp, ...)
 * This implements the HotOS '21 rpush primitive for general-purpose registers.
 */
static inline void seed_thread_reg(uint64_t thread_id, uint64_t reg_addr, uint64_t value) {
  uint64_t val = (value & 0x7FFFFFFFFFULL)
               | ((thread_id & 0x3ULL) << 39)
               | ((reg_addr & 0x1FULL) << 41);
  __asm__ volatile("csrw 0x083, %0" : : "r"(val) :);
}

/* Static stacks for threads 1, 2, 3 (4KB each).
 * Each thread needs its own stack since registers (including sp) start at 0
 * in a freshly initialized per-thread register file.
 * Thread 0 uses the stack set up by crt0 (_start).
 */
#define THREAD_STACK_WORDS 512  /* 512 * 8 bytes = 4KB */
static uint64_t thread1_stack[THREAD_STACK_WORDS];
static uint64_t thread2_stack[THREAD_STACK_WORDS];
static uint64_t thread3_stack[THREAD_STACK_WORDS];

/* Per-context result storage (in memory, shared across threads) */
typedef struct {
  uint64_t sum_result;
  uint64_t fib_result;
  uint64_t done;
} context_result_t;

static context_result_t context_results[NUM_CONTEXTS];

/* sum(0..99) = 4950 */
static uint64_t compute_sum(void) {
  // bp_print_string("[T");
  // bp_hprint_uint64(read_ctxt());
  // bp_print_string("] Computing sum(0..99)...\n");
  uint64_t sum = 0;
  for (uint64_t i = 0; i < 100; i++) {
    sum += i;
  }
  return sum;
}

/* fibonacci(15) = 610 */
static uint64_t compute_fibonacci(uint64_t n) {
  // bp_print_string("[T");
  // bp_hprint_uint64(read_ctxt());
  // bp_print_string("] Computing fibonacci(");
  // bp_hprint_uint64(n);
  // bp_print_string(")...\n");
  if (n <= 1) return n;
  uint64_t a = 0, b = 1;
  for (uint64_t i = 2; i <= n; i++) {
    uint64_t tmp = a + b;
    a = b;
    b = tmp;
  }
  return b;
}

/* Thread entry points for threads 1, 2, 3.
 * Each thread does its work, stores results, and switches back to thread 0.
 * Thread 0's NPC is already saved in context_storage from when it did
 * the csrw 0x081 to launch us — so switching back to 0 resumes thread 0
 * at the instruction after the csrw.
 */
void __attribute__((noinline)) thread1_entry(void) {

  // uint64_t sp;
  // __asm__ volatile("mv %0, sp" : "=r"(sp));
  // bp_print_string("[T1] Entry SP: ");
  // bp_hprint_uint64(sp);
  // bp_print_string("\n");

  context_results[1].sum_result = compute_sum();
  context_results[1].fib_result = compute_fibonacci(15);
  context_results[1].done = 1;
  write_ctxt(0);
  /* should not reach here */
  bp_finish(1);
}

void __attribute__((noinline)) thread2_entry(void) {
  context_results[2].sum_result = compute_sum();
  context_results[2].fib_result = compute_fibonacci(15);
  context_results[2].done = 1;
  write_ctxt(0);
  bp_finish(1);
}

void __attribute__((noinline)) thread3_entry(void) {
  context_results[3].sum_result = compute_sum();
  context_results[3].fib_result = compute_fibonacci(15);
  context_results[3].done = 1;
  write_ctxt(0);
  bp_finish(1);
}

static inline uint64_t read_cycle(void) {
  uint64_t cycles;
  __asm__ volatile("rdcycle %0" : "=r"(cycles) : :);
  return cycles;
}

uint64_t read_npc(void) {
  uint64_t npc;
  __asm__ volatile("csrr %0, 0x082" : "=r"(npc) : :);
  return npc;
}

int main(int argc, char** argv) {

  // if (read_ctxt() != 0) {
  //   bp_print_string("CRITICAL: Thread started at main instead of entry point!\n");

  //   // Print the Return Address (ra) to see who called main or where we came from
  //   uint64_t ra;
  //   __asm__ volatile("mv %0, ra" : "=r"(ra));
  //   bp_print_string("Faulty RA: ");
  //   bp_hprint_uint64(ra);
  //   bp_print_string("\n");

  //   bp_finish(1);
  // }

  bp_print_string("=== BlackParrot Phase 1.4 Context Switching Demo ===\n");
  bp_print_string("Testing CSR 0x081 (CTXT) + CSR 0x082 (NPC bootstrap)\n\n");

  /* Ensure we start on thread 0 */
  write_ctxt(0);

  /* Thread 0 does its own computation directly */
  context_results[0].sum_result = compute_sum();
  context_results[0].fib_result = compute_fibonacci(15);
  context_results[0].done = 1;

  uint64_t begin = read_cycle();

  /* Seed and launch threads 1, 2, 3 one at a time.
   * After each csrw 0x081, hardware redirects to that thread's entry.
   * That thread does work and returns via csrw 0x081, 0 — which resumes
   * thread 0 at the instruction after the csrw below. */

  // bp_print_string("Seeding and launching thread 1...\n");
  // bp_hprint_uint64((uint64_t)thread1_entry);
  // bp_print_string("\n");
  /* Seed thread 1's sp (x2) to the top of its static stack, then seed its entry NPC */
  seed_thread_reg(1, 2 /*x2=sp*/, (uint64_t)&thread1_stack[THREAD_STACK_WORDS]);
  seed_thread_npc(1, (uint64_t)thread1_entry);

  // bp_print_string("[T0] Switching to thread 1...\n");
  write_ctxt(1);
  // bp_print_string("[T0] Resumed from thread 1!\n");
  /* === Thread 0 resumes here after thread 1 calls write_ctxt(0) === */

  // bp_print_string("Seeding and launching thread 2...\n");
  // bp_hprint_uint64((uint64_t)thread2_entry);
  // bp_print_string("\n");
  /* Seed thread 2's sp (x2) to the top of its static stack, then seed its entry NPC */
  seed_thread_reg(2, 2 /*x2=sp*/, (uint64_t)&thread2_stack[THREAD_STACK_WORDS]);
  seed_thread_npc(2, (uint64_t)thread2_entry);

  // bp_print_string("[T0] Switching to thread 2...\n");
  write_ctxt(2);
  // bp_print_string("[T0] Resumed from thread 2!\n");
  /* === Thread 0 resumes here after thread 2 calls write_ctxt(0) === */

  // bp_print_string("Seeding and launching thread 3...\n");
  // bp_hprint_uint64((uint64_t)thread3_entry);
  // bp_print_string("\n");
  /* Seed thread 3's sp (x2) to the top of its static stack, then seed its entry NPC */
  seed_thread_reg(3, 2 /*x2=sp*/, (uint64_t)&thread3_stack[THREAD_STACK_WORDS]);
  seed_thread_npc(3, (uint64_t)thread3_entry);

  // bp_print_string("[T0] Switching to thread 3...\n");
  write_ctxt(3);
  // bp_print_string("[T0] Resumed from thread 3!\n");
  /* === Thread 0 resumes here after thread 3 calls write_ctxt(0) === */

  uint64_t end = read_cycle();

  /* Validate results */
  bp_print_string("\nValidation Results:\n");

  uint64_t expected_sum = 4950;
  uint64_t expected_fib = 610;
  int errors = 0;

  for (uint64_t ctx = 0; ctx < NUM_CONTEXTS; ctx++) {
    bp_print_string("Context ");
    bp_hprint_uint64(ctx);
    bp_print_string(": sum=");
    bp_hprint_uint64(context_results[ctx].sum_result);

    if (context_results[ctx].sum_result != expected_sum) {
      bp_print_string(" [FAIL]");
      errors++;
    }

    bp_print_string(", fib=");
    bp_hprint_uint64(context_results[ctx].fib_result);

    if (context_results[ctx].fib_result != expected_fib) {
      bp_print_string(" [FAIL]");
      errors++;
    }

    if (!context_results[ctx].done) {
      bp_print_string(" [NOT DONE]");
      errors++;
    }

    bp_print_string("\n");
  }

  bp_print_string("\nTotal cycles: ");
  bp_hprint_uint64(end - begin);
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
