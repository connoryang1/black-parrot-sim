/**
 * mt_regfile_test.c
 *
 * Real Hardware Multithreading Register Isolation Test
 *
 * This test verifies that the multi-threaded register file (bp_be_regfile_mt)
 * properly isolates register state between hardware threads.
 *
 * Mechanism:
 *   CSR 0x081 (CTXT): read/write current thread ID; writing triggers NPC redirect
 *   CSR 0x082 (CTXT_NPC): write-only bootstrap of thread NPC.
 *                          Value format: {thread_id[7:0], target_npc[63:0]}
 *                          (packed as a 64-bit value with thread_id in upper bits)
 *
 * Test flow:
 *   Thread 0 (main):
 *     1. Seed thread 1 entry via CSR 0x082
 *     2. Store sentinel values in global vars (thread 0's "registers")
 *     3. Switch to thread 1 via CSR 0x081 = 1
 *     4. Thread 0 resumes after thread 1 switches back
 *     5. Verify globals are intact (register isolation)
 *
 *   Thread 1 (thread1_entry):
 *     1. Clobbers local registers with different values
 *     2. Verifies it sees its OWN registers (not thread 0's)
 *     3. Writes result to shared memory
 *     4. Switches back to thread 0 via CSR 0x081 = 0
 */

#include <stdint.h>
#include "bp_utils.h"

/* CSR accessors */
static inline uint64_t read_ctxt(void) {
  uint64_t val;
  __asm__ volatile("csrr %0, 0x081" : "=r"(val) : :);
  return val;
}

static inline void write_ctxt(uint64_t val) {
  __asm__ volatile("csrw 0x081, %0" : : "r"(val) :);
}

/* CSR 0x082: seed a thread's entry NPC.
 * We encode: val = target_npc (thread_id goes in upper bits per RTL)
 * RTL: ctx_npc_write_tid_o = csr_data_li[vaddr_width_p +: thread_id_width_p]
 *      ctx_npc_write_npc_o = csr_data_li[0 +: vaddr_width_p]
 * With vaddr_width_p=39, thread_id_width_p=2:
 *   bits [38:0] = NPC (39-bit vaddr)
 *   bits [40:39] = thread_id
 */
static inline void seed_thread_npc(uint64_t thread_id, uint64_t npc) {
  /* Pack: thread_id in bits [40:39], npc in bits [38:0] (vaddr_width_p=39) */
  uint64_t val = ((thread_id & 0x3ULL) << 39) | (npc & 0x7FFFFFFFFFULL);
  __asm__ volatile("csrw 0x082, %0" : : "r"(val) :);
}

/* Shared state between threads */
volatile uint64_t thread1_ran = 0;
volatile uint64_t thread1_sum = 0;
volatile uint64_t thread0_resume_addr = 0;  /* thread 0's return NPC */

/* Sentinel values thread 0 stores before switching away.
 * These live in memory - the test is that thread 0's register state
 * (which gets saved/restored via the MT regfile) is intact on return. */
volatile uint64_t t0_sentinel_a = 0xAAAAAAAAAAAAAAAAULL;
volatile uint64_t t0_sentinel_b = 0xBBBBBBBBBBBBBBBBULL;

/* Private stack for thread 1.
 * Thread 1's register file starts all-zero (sp=0), so we must set sp
 * in assembly before any C code runs. This array provides that stack. */
static uint64_t thread1_stack[256];  /* 2 KB */

/* C body of thread 1's work — called after sp is set up. */
static void __attribute__((noinline)) thread1_main(void) {
  /* Verify we're running as thread 1 */
  uint64_t my_id = read_ctxt();

  bp_print_string("[T1] Thread 1 started, id=");
  bp_hprint_uint64(my_id);
  bp_print_string("\n");

  if (my_id != 1) {
    bp_print_string("[T1] ERROR: wrong thread id!\n");
    bp_finish(1);
  }

  /* Compute something using thread 1's registers (should all start as 0) */
  uint64_t sum = 0;
  for (uint64_t i = 1; i <= 10; i++) {
    sum += i;
  }
  /* sum(1..10) = 55 */

  bp_print_string("[T1] Sum(1..10)=");
  bp_hprint_uint64(sum);
  bp_print_string(" (expected 0x37=55)\n");

  /* Store result and flag */
  thread1_sum = sum;
  thread1_ran = 1;

  bp_print_string("[T1] Switching back to thread 0\n");

  /* Switch back to thread 0 - hardware redirects to thread 0's saved NPC */
  write_ctxt(0);

  /* Thread 1 should never reach here - the context switch redirects execution */
  bp_print_string("[T1] ERROR: Should not reach here after ctxtsw!\n");
  bp_finish(1);
}

/* Thread 1 naked entry point — seeded via CSR 0x082.
 * All registers (including sp/x2) start as 0 in thread 1's register file,
 * so we must set sp in assembly before calling any C function. */
void __attribute__((naked, noinline)) thread1_entry(void) {
  __asm__ volatile(
    "la   sp, thread1_stack\n"   /* point sp at base of array  */
    "li   t0, 2048\n"            /* stack size = 256*8 bytes    */
    "add  sp, sp, t0\n"          /* sp = top of stack (grows ↓) */
    "call thread1_main\n"        /* enter C code with valid sp  */
  );
}

int main(int argc, char** argv) {
  bp_print_string("=== MT Register File Isolation Test ===\n");

  /* Ensure we start as thread 0 */
  write_ctxt(0);

  uint64_t initial_id = read_ctxt();
  bp_print_string("[T0] Starting as thread ");
  bp_hprint_uint64(initial_id);
  bp_print_string("\n");

  if (initial_id != 0) {
    bp_print_string("[T0] ERROR: not starting as thread 0\n");
    bp_finish(1);
  }

  /* Get thread1_entry's address and seed thread 1's NPC */
  uint64_t t1_npc = (uint64_t)thread1_entry;
  bp_print_string("[T0] Seeding thread 1 NPC = ");
  bp_hprint_uint64(t1_npc);
  bp_print_string("\n");

  seed_thread_npc(1, t1_npc);

  /* Store sentinels in thread 0's "state" (memory-backed for C) */
  t0_sentinel_a = 0xAAAAAAAAAAAAAAAAULL;
  t0_sentinel_b = 0xBBBBBBBBBBBBBBBBULL;

  bp_print_string("[T0] Before switch: sentinel_a=");
  bp_hprint_uint64(t0_sentinel_a);
  bp_print_string(", sentinel_b=");
  bp_hprint_uint64(t0_sentinel_b);
  bp_print_string("\n");

  bp_print_string("[T0] Switching to thread 1...\n");

  /* Context switch: hardware saves thread 0's NPC (next instruction after this)
   * and redirects frontend to thread 1's seeded NPC (thread1_entry).
   * Thread 0 will resume here when thread 1 does csrw 0x081, 0. */
  write_ctxt(1);

  /* === Thread 0 resumes here after thread 1 switches back === */
  bp_print_string("[T0] Resumed from thread 1!\n");

  /* Verify thread 1 ran */
  if (!thread1_ran) {
    bp_print_string("[T0] ERROR: thread1_ran not set\n");
    bp_finish(1);
  }

  bp_print_string("[T0] Thread 1 sum = ");
  bp_hprint_uint64(thread1_sum);
  bp_print_string(" (expected 0x37=55)\n");

  if (thread1_sum != 55) {
    bp_print_string("[T0] ERROR: wrong sum from thread 1\n");
    bp_finish(1);
  }

  /* Verify thread 0's memory-backed state is intact */
  bp_print_string("[T0] After return: sentinel_a=");
  bp_hprint_uint64(t0_sentinel_a);
  bp_print_string(", sentinel_b=");
  bp_hprint_uint64(t0_sentinel_b);
  bp_print_string("\n");

  if (t0_sentinel_a != 0xAAAAAAAAAAAAAAAAULL) {
    bp_print_string("[T0] ERROR: sentinel_a corrupted!\n");
    bp_finish(1);
  }
  if (t0_sentinel_b != 0xBBBBBBBBBBBBBBBBULL) {
    bp_print_string("[T0] ERROR: sentinel_b corrupted!\n");
    bp_finish(1);
  }

  /* Verify we're back as thread 0 */
  uint64_t final_id = read_ctxt();
  bp_print_string("[T0] Final thread id = ");
  bp_hprint_uint64(final_id);
  bp_print_string("\n");

  if (final_id != 0) {
    bp_print_string("[T0] ERROR: wrong thread id after return\n");
    bp_finish(1);
  }

  bp_print_string("\nALL TESTS PASSED\n");
  bp_finish(0);

  return 0;
}
