# Black Parrot: Incremental Implementation Plan for Hardware Thread Management
## Eliminating Context Switches Through Software-Managed Threads

**Based on**: "A Case Against (Most) Context Switches" (Humphries et al., HotOS 2021)

**Goal**: Enable Black Parrot to support 10s-1000s of software-managed hardware threads per core without context switching overhead.

**Timeline**: 32 weeks (8 phases) | **Team**: 4-6 engineers | **Complexity**: High

---

## Executive Summary

### Vision
Transform Black Parrot from single-threaded (1 thread/core) to software-managed many-threaded:
- **Before**: 1 hardware thread, context-switched software threads (expensive)
- **After**: 100s of hardware threads, directly managed by software (fast)

### Key Insight
Instead of the OS context-switching a single hardware thread, **keep many hardware threads resident** and let software start/stop them. This eliminates:
- ❌ Interrupt dispatch overhead
- ❌ Register save/restore cycles
- ❌ TLB/cache thrashing
- ❌ Scheduler invocation latency

### Hardware Cost (Per Core)
```
Register file:      64 KB (100 threads × 32 regs × 64-bit)
Thread context:      8 KB (100 threads × 64 bytes state)
Scheduler logic:      2 KB gates
Monitor tracking:     4 KB CAM
TDT storage:          1 KB
─────────────────────────────
Total:             ~79 KB (scales linearly with thread count)
```

### Software Benefits
- **No-interrupt I/O**: monitor/mwait on RX queue
- **Exception-less syscalls**: Start syscall handler thread, no mode switch
- **Fast microkernel calls**: Direct thread-to-thread control
- **Hypervisor isolation**: Untrusted code in separate threads
- **Simple distributed code**: One thread per request, blocking I/O

---

## Phase 0: Preparation (Weeks 1-2)

### Objectives
- Establish baseline for changes
- Design thread state layout
- Create test infrastructure
- Define new CSRs

### Step 0.1: Define Thread State Structure

**What**: Design the 64-byte per-thread context that will be stored in expanded register file

**How**:
1. Create new SystemVerilog package: `bp_common_thread_state.svh`
2. Define `bp_thread_context_s` struct:

```systemverilog
typedef struct packed {
    logic [63:0] pc;              // Program counter (64 bits)
    logic [63:0] npc;             // Next PC (64 bits)
    logic [7:0]  privilege;       // privilege mode (M/S/U) + other flags (8 bits)
    logic [15:0] global_history;  // Branch prediction history (16 bits)
    logic [7:0]  thread_id;       // Virtual thread ID (8 bits)
    logic [63:0] exception_ptr;   // Exception descriptor address (64 bits)
    logic [63:0] tdt_ptr;         // Thread Descriptor Table pointer (64 bits)
    logic [63:0] thread_local;    // Thread-local storage / descriptor (64 bits)
    logic [31:0] status;          // Thread status + control flags (32 bits)
    logic [63:0] reserved;        // For future use (64 bits)
} bp_thread_context_s;  // Total: 544 bits = 68 bytes
```

**Why**: Defines what state each thread must keep. Each thread gets one context struct, stored alongside its registers.

**Location**: Create file `black-parrot/bp_common/src/v/bp_common_thread_state.svh`

**Dependencies**: Must be included in all modules that touch thread state

**Validation**: Verify struct size ≤ 68 bytes using `$bits()`

---

### Step 0.2: Define New CSRs (Control and Status Registers)

**What**: New CSRs for managing threads that software will use

**How**:
1. Extend `bp_common_rv64_pkgdef.svh` with new CSR definitions:

```systemverilog
// New CSR addresses for thread management (0xC00-0xCFF range)
parameter csr_addr_t CSR_PTID         = 12'hC00;  // Physical Thread ID (read-only)
parameter csr_addr_t CSR_VTID         = 12'hC01;  // Virtual Thread ID (read-write)
parameter csr_addr_t CSR_THREAD_STAT  = 12'hC02;  // Thread status flags
parameter csr_addr_t CSR_EXCEPTION_PTR = 12'hC03; // Exception descriptor pointer
parameter csr_addr_t CSR_TDT_PTR      = 12'hC04;  // Thread Descriptor Table pointer
parameter csr_addr_t CSR_MONITOR_ADDR = 12'hC05;  // Monitor address (for mwait)
parameter csr_addr_t CSR_THREAD_LOCAL = 12'hC06;  // Thread-local storage
parameter csr_addr_t CSR_NUM_THREADS  = 12'hC07;  // Number of available threads (read-only)
```

**Why**: Software needs these to:
- Query current thread identity
- Set up thread context
- Configure exception handling
- Establish monitor addresses

**Location**: Modify `black-parrot/bp_common/src/v/bp_common_rv64_pkgdef.svh` (add to existing csr definitions)

**Validation**: No CSR conflicts with existing RV64 spec

---

### Step 0.3: Establish Test Harness

**What**: Create SystemVerilog testbench to verify threading features incrementally

**How**:
1. Create `bp_thread_test.sv` with:
   - Simple thread spawner (calls start instruction)
   - Register write/read checker
   - Exception notification validator
   - Monitor address checker

2. Create simple test program in RISC-V assembly:
```asm
# test_basic_thread.s
# Start thread 1, let it execute 10 instructions, check results

main:
    # Enable thread 1
    li x1, 1              # vtid = 1
    invtid x0, x1         # Invalidate TDT (required after setup)
    start x1              # Start virtual thread 1

    # Main thread loop
    li x2, 100
loop:
    addi x2, x2, -1
    bne x2, x0, loop

    # Stop thread 1
    stop x1

    # Check thread 1 results
    rpull x5, x1, x10     # Read register x10 from thread 1

    # Verify correct result
    beq x5, x12, pass
    j fail
```

**Why**: Enables testing each phase immediately without waiting for software/kernel

**Location**: Create `black-parrot/bp_common/test/py/bp_thread_test.py` or similar

**Validation**: Testbench compiles, though tests will fail until implementation

---

### Step 0.4: Extend Configuration System

**What**: Add threading parameters to Black Parrot configuration

**How**:
1. Modify `bp_common_aviary_cfg_pkgdef.svh`:

```systemverilog
typedef struct packed {
    // ... existing fields ...

    // Thread support parameters
    logic [7:0]  num_threads_gp;           // Number of hardware threads (default: 64)
    logic [7:0]  thread_context_bytes_gp;  // Bytes per thread context (default: 68)
    logic        thread_scheduler_gp;      // Enable hardware scheduler (default: 1)
    logic        monitor_mwait_gp;         // Enable monitor/mwait (default: 1)
    logic        exception_to_thread_gp;   // Exception disables thread (default: 1)
} bp_proc_param_s;
```

2. Modify `bp_common_aviary_defines.svh` to derive new parameters:
```systemverilog
`define thread_regfile_size_gp  (num_threads_gp * 32 * 64)  // bits
`define thread_context_size_gp  (num_threads_gp * thread_context_bytes_gp)  // bytes
```

**Why**: Allows different configurations (8 threads, 64 threads, 256 threads) without code changes

**Location**: Modify existing files in `black-parrot/bp_common/src/v/`

**Validation**: Existing configs still work, new configs parse correctly

---

## Phase 1: Expanded Register File (Weeks 3-6)

### Objectives
- Design and implement 8K×64-bit 3R1W SRAM (vs current 32×64-bit)
- Add thread ID indexing
- Integrate with decode/dispatch
- Close timing at target frequency

### Critical Path Item ⚠️
**Register file is highest risk** - Multi-port SRAM timing closure must succeed before continuing

---

### Step 1.1: Design Thread-Indexed Register File

**What**: Replace 32-entry single-thread regfile with 8K-entry banked regfile indexed by {thread_id, reg_addr}

**Current Implementation** (`bp_be_regfile.sv`, ~160 lines):
```systemverilog
// Current: 32 entries, single thread
bsg_mem_2r1w_sync #(.width_p(64), .els_p(32)) int_rf (
    .r0_addr_i(rs1_addr),      // 5 bits
    .r1_addr_i(rs2_addr),      // 5 bits
    .w_addr_i(rd_addr),        // 5 bits
    // ...
);
```

**New Implementation**:
```systemverilog
// New: 8K entries (8 threads × 1K, or 256 threads × 32 entries, etc.)
// Indexed as: flat_addr = {thread_id[num_thread_bits-1:0], reg_addr[4:0]}

typedef struct packed {
    logic [num_thread_bits-1:0] tid;
    logic [4:0] rid;
} regfile_addr_s;

bsg_mem_3r1w_sync #(
    .width_p(64),
    .els_p(8192)              // 8K entries
) int_rf (
    .r0_addr_i({rs1_tid, rs1_addr}),  // Thread 1 reg
    .r1_addr_i({rs2_tid, rs2_addr}),  // Thread 2 reg (can read from different thread!)
    .r2_addr_i({rs3_tid, rs3_addr}),  // Thread 3 reg
    .w_addr_i({rd_tid, rd_addr}),     // Write
    // ...
);
```

**Why**:
- ✅ Area scales linearly: O(thread_count)
- ✅ Simpler timing than multi-threaded SMT (no arbitration)
- ✅ One active thread per cycle reads as fast as original
- ✅ Enables rpull/rpush (read from other threads)

**How** (Implementation Steps):
1. **Create new module**: `bp_be_regfile_mt.sv` (MT = multi-thread)
   - Copy from existing `bp_be_regfile.sv`
   - Change address width: 5 bits → 13 bits (8 thread bits + 5 reg bits)
   - Change SRAM size: 32 → 8192 entries
   - Add thread_id muxes on read ports
   - Keep write port single (active thread only)

2. **Update `bp_be_top.sv`**:
   - Replace instantiation of `bp_be_regfile` with `bp_be_regfile_mt`
   - Pass `num_threads_gp` parameter from config
   - Thread ID comes from current dispatch packet

3. **Update address generation in `bp_be_dispatch.sv`**:
   - Add thread ID to register address calculations
   - Route thread_id through: decode → dispatch → regfile

**Dependencies**:
- Must have current regfile working perfectly (Phase 0)
- BSG SRAM compiler available for 8K×64 3R1W

**Timeline**:
- Week 3: Design, simulation (single-thread)
- Week 4: Synthesis, timing analysis
- Week 5: Timing closure (may require pipelining)
- Week 6: Integration with pipeline

**Validation**:
```systemverilog
// Testbench: Verify no data corruption when accessing different threads
for (int i = 0; i < NUM_THREADS; i++) begin
    regfile.write(8'(i), 5'd1, 64'(i*1000 + 1));
end
for (int i = 0; i < NUM_THREADS; i++) begin
    value = regfile.read(8'(i), 5'd1);
    assert (value == 64'(i*1000 + 1)) else $error("Corruption!");
end
```

**Success Criteria**:
- ✅ Passes simulation with 64 threads
- ✅ Synthesizes at target frequency (e.g., 1 GHz)
- ✅ Timing margin ≥ 300 ps
- ✅ Single-threaded performance unchanged

---

### Step 1.2: Distributed Thread Context Storage

**What**: Store 64-byte context per thread in distributed flip-flops near the scheduler/frontend

**Why Distributed?**:
- Thread context (PC, ghist, privilege) is too large for dedicated SRAM
- But it's accessed ONLY during thread switches (infrequent)
- Distributed FF storage is faster to access than another SRAM

**How**:
1. Create new module: `bp_be_thread_context_storage.sv`

```systemverilog
module bp_be_thread_context_storage #(
    parameter num_threads_gp = 64,
    parameter data_width_p = 544  // bits (68 bytes)
) (
    input clk_i,
    input reset_i,

    // Write context (during start/stop)
    input [7:0] write_tid_i,
    input [data_width_p-1:0] write_data_i,
    input write_en_i,

    // Read context (during thread switch)
    input [7:0] read_tid_i,
    output [data_width_p-1:0] read_data_o,

    // Direct field access (used during execution)
    input [7:0] active_tid_i,
    output [63:0] pc_o,                // Program counter
    output [15:0] global_history_o,    // Branch history
    output [7:0]  privilege_o,         // Privilege level
    output [63:0] exception_ptr_o,     // Exception handler
    output [63:0] tdt_ptr_o            // Thread descriptor table
);

    // One context per thread, stored in distributed FFs
    bp_thread_context_s contexts_r [num_threads_gp-1:0];

    // Write side
    always @(posedge clk_i)
        if (write_en_i)
            contexts_r[write_tid_i] <= write_data_i;

    // Read sides (multi-port reads)
    assign read_data_o = contexts_r[read_tid_i];

    // For active thread, broadcast frequently-used fields
    assign pc_o = contexts_r[active_tid_i].pc;
    assign global_history_o = contexts_r[active_tid_i].global_history;
    assign privilege_o = contexts_r[active_tid_i].privilege;
    assign exception_ptr_o = contexts_r[active_tid_i].exception_ptr;
    assign tdt_ptr_o = contexts_r[active_tid_i].tdt_ptr;

endmodule
```

**Where**: Place near pipeline front-end (after BTB/BHT, before instruction fetch)

**Integration**:
- Create in new file: `black-parrot/bp_be/src/v/bp_be_checker/bp_be_thread_ctx_storage.sv`
- Instantiate in `bp_fe_top.sv` (for PC/branch history)
- Connect to `bp_be_scheduler.sv` (for exception handling)

**Timeline**: Week 3 (parallel with Step 1.1)

**Validation**:
- ✅ Can write 64 different contexts without interference
- ✅ Can read from one thread while active thread runs different
- ✅ Fields broadcast correctly to pipeline

**Success Criteria**:
- ✅ No timing violations (distributed FFs are fast)
- ✅ Proven data integrity
- ✅ Less than 300 gates overhead

---

### Step 1.3: Thread ID Routing Through Pipeline

**What**: Pass thread_id as additional field through decode → dispatch → execute → writeback

**Current State**: No thread_id in decode packet (assumes single thread)

**How**:
1. **Modify decode packet** in `bp_be_instr_decoder.sv`:

```systemverilog
// Current
typedef struct packed {
    // ... existing fields ...
    logic [4:0] rd_addr;
    logic [4:0] rs1_addr;
} bp_be_decode_s;

// New
typedef struct packed {
    // ... existing fields ...
    logic [4:0]   rd_addr;
    logic [4:0]   rs1_addr;
    logic [7:0]   thread_id;      // ADD THIS
} bp_be_decode_s;
```

2. **Update decode module**: Route thread_id from instruction fetch:
```systemverilog
// In bp_be_instr_decoder.sv
module bp_be_instr_decoder (
    // ... existing ports ...
    input [7:0] thread_id_i,      // From scheduler
    output bp_be_decode_s decode_o,
);

    always_comb begin
        decode_o.thread_id = thread_id_i;  // Propagate
        // ... rest of decode logic ...
    end
endmodule
```

3. **Update dispatch packet** similarly:
```systemverilog
// In bp_be_scheduler.sv
typedef struct packed {
    // ... existing fields ...
    logic [7:0] thread_id;        // ADD THIS
} bp_be_dispatch_packet_s;
```

4. **Update all downstream modules**:
   - `bp_be_execute.sv`: Use thread_id for register write
   - `bp_be_writeback.sv`: Use thread_id for commit
   - `bp_be_regfile_mt.sv`: Already designed for this

**Why**:
- Enables regfile to know which thread is writing
- Allows exceptions to disable correct thread
- Sets up for future: reading from different threads (rpull)

**Timeline**: Week 3 (parallel, ~1 day work)

**Validation**:
- ✅ All instructions complete with same thread_id
- ✅ No cross-thread corruption
- ✅ Single-threaded performance unchanged

**Success Criteria**:
- ✅ All tests pass with thread_id added to pipeline
- ✅ No timing degradation
- ✅ Thread ID correctly routed end-to-end

---

### Step 1.4: Timing Closure and Integration

**What**: Close timing paths for expanded regfile and thread ID routing

**Timeline**: Weeks 4-6

**Process**:
1. **Synthesis** (Week 4):
   - Run full synthesis with new regfile size
   - Report timing violations
   - Identify critical paths

2. **Timing Issues** (Likely):
   - 3R1W SRAM timing might not close at target frequency
   - Solution options:
     a) **Pipeline the regfile** (add register stage): +1 cycle latency
     b) **Use heterogeneous memory** (fast small SRAM for active thread, large DRAM-like for others)
     c) **Reduce threads to fit smaller SRAM** (e.g., 256 threads instead of 8K)

3. **Pipelining Example** (if needed):
```systemverilog
// Before: Regfile read available in same cycle
reg_data = regfile.read(addr);

// After: Regfile read available next cycle
reg_addr_r <= addr;
reg_data <= regfile.read(reg_addr_r);  // Delayed by 1 cycle
```

4. **Integration** (Weeks 5-6):
   - Add pipeline stage to decode → dispatch if needed
   - Update scheduler to account for latency
   - Verify end-to-end system timing

**Success Criteria**:
- ✅ Synthesizes at 1 GHz (or target frequency)
- ✅ Setup/hold met on all paths
- ✅ No performance regression vs baseline

---

## Phase 2: Thread Control Instructions (Weeks 7-10)

### Objectives
- Implement start/stop instructions
- Add thread enable/disable logic in hardware
- Implement Thread Descriptor Table (TDT) permission checks
- Enable/disable threads based on privilege

### Step 2.1: Implement start/stop Instructions

**What**: New instructions to enable/disable threads

**ISA Format**:
```
start <vtid>      # opcode: custom-0, funct7: 0x00
stop <vtid>       # opcode: custom-0, funct7: 0x01
```

**How**:
1. **Add to instruction decoder** (`bp_be_instr_decoder.sv`):

```systemverilog
// In the instruction decode casez
always_comb begin
    case (instr[6:0])
        // ... existing opcodes ...
        7'b0001011: begin  // custom-0 opcode (officially extension)
            case (instr[14:12])
                3'b000: begin  // start <vtid>
                    pipe_sel = START_PIPE;    // Route to new START execution unit
                    rd_valid = 1'b0;          // No register destination
                    rs1_valid = 1'b1;         // vtid in rs1
                    instr_type = START;
                end
                3'b001: begin  // stop <vtid>
                    pipe_sel = STOP_PIPE;
                    rd_valid = 1'b0;
                    rs1_valid = 1'b1;         // vtid in rs1
                    instr_type = STOP;
                end
            endcase
        end
    endcase
end
```

2. **Create thread control execution unit** (`bp_be_thread_control.sv`):

```systemverilog
module bp_be_thread_control (
    input clk_i,
    input reset_i,

    // From dispatch
    input bp_be_dispatch_packet_s dispatch_i,

    // Thread context interface
    input [7:0] vtid_to_start_i,
    input [7:0] vtid_to_stop_i,
    output [7:0] ptid_enabled_o [0:num_threads_gp-1],

    // TDT (Thread Descriptor Table) interface
    input [63:0] tdt_ptr_i,        // From active thread context
    output logic permission_valid_o,

    // To writeback
    output bp_be_writeback_packet_s writeback_o
);

    // Thread enable register (1 = runnable, 0 = disabled)
    logic [num_threads_gp-1:0] thread_enabled_r;

    // TDT entry structure
    typedef struct packed {
        logic [7:0]  target_ptid;
        logic [3:0]  permissions;     // [start, stop, modify_regs, modify_csrs]
    } tdt_entry_s;

    // Handle start instruction
    always @(posedge clk_i) begin
        if (dispatch_i.instr_type == START) begin
            // Check TDT permissions
            if (check_permission(dispatch_i.thread_id, vtid_to_start_i, START_PERM)) begin
                thread_enabled_r[vtid_to_start_i] <= 1'b1;  // Enable thread
            end else begin
                // Generate exception: permission denied
                writeback_o.exception_valid = 1'b1;
            end
        end
    end

    // Handle stop instruction
    always @(posedge clk_i) begin
        if (dispatch_i.instr_type == STOP) begin
            // Check permissions
            if (check_permission(dispatch_i.thread_id, vtid_to_stop_i, STOP_PERM)) begin
                thread_enabled_r[vtid_to_stop_i] <= 1'b0;  // Disable thread
            end else begin
                writeback_o.exception_valid = 1'b1;
            end
        end
    end

    // Output enabled threads to scheduler
    assign ptid_enabled_o = thread_enabled_r;

endmodule
```

**Location**: Create new file `black-parrot/bp_be/src/v/bp_be_checker/bp_be_thread_control.sv`

**Integration**:
- Route to execution pipeline from `bp_be_scheduler.sv`
- Return exception from `bp_be_writeback.sv` if permission denied
- Update thread enable register in `bp_be_thread_ctx_storage.sv`

**Timeline**: Week 7-8

**Validation**:
- ✅ start instruction enables thread for execution
- ✅ stop instruction disables thread
- ✅ Only one active thread executes per cycle (scheduler enforces)
- ✅ Disabled threads don't execute

---

### Step 2.2: Implement Thread Descriptor Table (TDT)

**What**: Permission structure (in memory) that controls which threads can manage which threads

**Format** (in memory):
```
TDT Entry (at base + vtid*16 bytes):
  Offset 0-7:  ptid (physical thread ID) + flags (8 bytes)
  Offset 8-15: permissions + reserved (8 bytes)

Permissions bits [3:0]:
  [0]: Can start this thread
  [1]: Can stop this thread
  [2]: Can modify general registers
  [3]: Can modify control registers (CSRs)
```

**Example**:
```
# Memory layout for 4 threads
TDT_Base = 0x2000

TDT[0] @ 0x2000: ptid=0x00, perms=0b1111 (full permissions)
TDT[1] @ 0x2010: ptid=0x01, perms=0b1100 (can't control)
TDT[2] @ 0x2020: ptid=0x02, perms=0b0011 (thread 0 can start/stop)
TDT[3] @ 0x2030: ptid=0x03, perms=0b0000 (disabled)
```

**How to Implement**:

1. **CSR for TDT base pointer** (already defined in Phase 0):
   - `CSR_TDT_PTR` (0xC04) = base address of TDT in memory

2. **Permission check logic** in `bp_be_thread_control.sv`:

```systemverilog
function logic check_permission(
    input [7:0] source_tid,      // Thread trying to start/stop
    input [7:0] target_vtid,     // Thread being started/stopped
    input [1:0] perm_type        // START_PERM or STOP_PERM
);
    // Read TDT from memory
    // tdt_ptr comes from source thread's context
    // tdt_entry = memory[tdt_ptr + target_vtid * 16]
    // Return (tdt_entry.permissions >> perm_type) & 1'b1

    // For now, simplified: kernel (tid=0) can control all threads
    return (source_tid == 8'd0) || ((tdt_ptr != 64'b0) &&
            (read_mem(tdt_ptr + (target_vtid << 4)) & (1 << perm_type)));
endfunction
```

3. **Update CSR handling** in `bp_be_csr.sv`:
   - Add write handler for `CSR_TDT_PTR`
   - Store in active thread's context

4. **invtid instruction** (TDT invalidation):
   - Required after TDT changes (flushes hardware caches)
   - Specification from paper:
```
invtid <vtid>, <remote_vtid>
```
   - Invalidates any cached TDT entries for the thread
   - Needed if kernel changes TDT in memory

**Location**: Modify `bp_be_thread_control.sv` and `bp_be_csr.sv`

**Timeline**: Week 8-9

**Validation**:
- ✅ Kernel (tid=0) can start all threads
- ✅ User thread can't start without permission
- ✅ invtid flushes cached permissions

---

### Step 2.3: privilege Mode and Exception Handling Integration

**What**: Ensure thread control operations respect privilege levels

**How**:
1. **Privilege check in start/stop**:
   - Only supervisor mode (privilege >= 1) can start/stop threads
   - User mode threads that attempt start/stop get exception

```systemverilog
if (dispatch_i.instr_type == START) begin
    if (privilege_i < SUPERVISOR_MODE) begin
        // Privilege exception
        writeback_o.exception_valid = 1'b1;
        writeback_o.exception_code = PRIVILEGE_FAULT;
    end else if (!check_tdt_permission(...)) begin
        // Permission exception
        writeback_o.exception_valid = 1'b1;
        writeback_o.exception_code = ACCESS_FAULT;
    end else begin
        thread_enabled_r[vtid] <= 1'b1;
    end
end
```

2. **Update CSR access control**:
   - Reading/writing `CSR_PTID`, `CSR_NUM_THREADS` allowed from user mode
   - Reading/writing `CSR_TDT_PTR`, `CSR_EXCEPTION_PTR` requires supervisor mode

**Timeline**: Week 9

**Success Criteria**:
- ✅ User mode can't execute start/stop
- ✅ User mode gets proper exception
- ✅ Privilege bits correctly propagate through context

---

## Phase 3: Register Access (rpull/rpush) (Weeks 11-13)

### Objectives
- Implement rpull (read remote register)
- Implement rpush (write remote register)
- Enable thread context swapping without memory operations

### Step 3.1: Implement rpull/rpush Instructions

**What**: Allow one thread to read/write another thread's registers

**ISA Format**:
```
rpull <rd>, <vtid>, <rs_src>
       Read register <rs_src> from thread <vtid> into <rd>

rpush <vtid>, <rd_dst>, <rs>
       Write register <rs> into register <rd_dst> of thread <vtid>
```

**Why**: Enables thread context swapping without memory operations:
```asm
# Swap software thread from thread 1 to thread 2
stop x1              # Stop thread 1
rpull x2, x1, x10    # Read thread 1's x10
rpush x2, x10, x11   # Write to thread 2's x10
start x2             # Start thread 2
```

**Implementation**:

1. **Update instruction decoder** (add to instruction decode):

```systemverilog
// rpull/rpush use R-type format with new funct7
case (instr[6:0])
    7'b0001100: begin  // rpull/rpush opcode
        case (instr[14:12])
            3'b000: begin  // rpull rd, vtid, rs_src
                pipe_sel = RPULL_PIPE;
                rd_valid = 1'b1;        // Write rd
                rs1_valid = 1'b0;
                instr_type = RPULL;
                // Operands:
                //   rs_src = instr[19:15]  (register # to read from remote thread)
                //   vtid = instr[24:20]    (target thread)
                //   rd = instr[11:7]       (destination)
            end
            3'b001: begin  // rpush vtid, rd_dst, rs
                pipe_sel = RPUSH_PIPE;
                rd_valid = 1'b0;
                rs1_valid = 1'b1;
                instr_type = RPUSH;
                // Operands:
                //   rs = instr[19:15]   (value to write)
                //   rd_dst = instr[14:12] (register # to write to)
                //   vtid = instr[24:20]   (target thread)
            end
        endcase
    end
endcase
```

2. **Create rpull/rpush execution unit** (`bp_be_regfile_access.sv`):

```systemverilog
module bp_be_regfile_access (
    input clk_i,
    input reset_i,

    // From dispatch
    input bp_be_dispatch_packet_s dispatch_i,
    input [63:0] data_i,           // Data to write (for rpush)

    // Register file interface
    output [7:0] read_tid_o,
    output [4:0] read_addr_o,
    input [63:0] read_data_i,

    output [7:0] write_tid_o,
    output [4:0] write_addr_o,
    output [63:0] write_data_o,
    output write_enable_o,

    // To writeback
    output bp_be_writeback_packet_s writeback_o,
    output [63:0] result_o
);

    always_comb begin
        if (dispatch_i.instr_type == RPULL) begin
            // Read from remote thread
            read_tid_o = dispatch_i.rs1[7:0];    // vtid
            read_addr_o = dispatch_i.rs1_addr;   // register #
            write_enable_o = 1'b0;               // Don't modify register file
            result_o = read_data_i;              // Return remote register value
        end else if (dispatch_i.instr_type == RPUSH) begin
            // Write to remote thread
            write_tid_o = dispatch_i.rs1[7:0];   // vtid
            write_addr_o = dispatch_i.rd_addr;   // register #
            write_data_o = data_i;               // Value to write
            write_enable_o = 1'b1;               // Enable write
        end
    end

endmodule
```

3. **Privilege check**:
   - rpull/rpush require supervisor mode (same as start/stop)
   - Thread trying to access must have permission in TDT

**Location**: Create `black-parrot/bp_be/src/v/bp_be_checker/bp_be_regfile_access.sv`

**Timeline**: Week 11-12

**Validation**:
- ✅ rpull reads disabled thread's register
- ✅ rpush writes disabled thread's register
- ✅ Active thread unaffected
- ✅ Permission checks enforced

---

### Step 3.2: Enable Context Field Access via rpull/rpush

**What**: Extend rpull/rpush to access thread context fields (PC, exception_ptr, etc.)

**How**:
- Use special register IDs (above 31, which is max for GPRs)
- rpull/rpush with rid ≥ 32 accesses context fields:
  - rid=32: PC
  - rid=33: NPC
  - rid=34: Global history
  - rid=35: Privilege level
  - rid=36: Exception pointer
  - rid=37: TDT pointer
  - rid=38: Thread-local storage
  - etc.

**Implementation**:

```systemverilog
always_comb begin
    if (dispatch_i.instr_type == RPULL) begin
        reg_id = dispatch_i.rs1_addr;
        if (reg_id < 5'd32) begin
            // GPR access (normal register)
            read_tid_o = dispatch_i.rs1[7:0];
            read_addr_o = reg_id;
            result_o = read_data_i;
        end else begin
            // Context field access
            case (reg_id)
                5'd32: result_o = contexts[vtid].pc;
                5'd33: result_o = contexts[vtid].npc;
                5'd34: result_o = {{48{1'b0}}, contexts[vtid].global_history};
                5'd35: result_o = {{56{1'b0}}, contexts[vtid].privilege};
                5'd36: result_o = contexts[vtid].exception_ptr;
                5'd37: result_o = contexts[vtid].tdt_ptr;
                5'd38: result_o = contexts[vtid].thread_local;
                default: result_o = 64'b0;
            endcase
        end
    end
end
```

**Timeline**: Week 12 (parallel with Step 3.1)

**Success Criteria**:
- ✅ Can read/write thread PC via rpull/rpush
- ✅ Can read/write exception pointer
- ✅ Context fields correctly transferred

---

## Phase 4: Monitor and mwait (Weeks 14-16)

### Objectives
- Implement monitor instruction (set address to monitor)
- Implement mwait instruction (block until monitored address written)
- Thread wakeup on write detection
- Support for I/O waiting and inter-thread communication

### Critical Feature ⚠️
**Monitor/mwait enables:**
- No-interrupt I/O: Thread waits on RX queue tail
- No-exception I/O: Thread blocks on page fault address
- Inter-thread communication: Threads wake each other via memory

---

### Step 4.1: Monitor Address CAM Implementation

**What**: Hardware to track which addresses each thread is monitoring

**Architecture**: CAM (Content Addressable Memory) in L1 D-cache

**How**:
1. **Add monitor tracking to L1 D-cache** (`bp_be_dcache.sv`):

```systemverilog
// One entry per thread
typedef struct packed {
    logic [63:0] address;
    logic        valid;
    logic [7:0]  thread_id;
} monitor_entry_s;

monitor_entry_s monitor_cam [num_threads_gp-1:0];

// On any L1 cache write (hit or miss)
always @(posedge clk_i) begin
    for (int i = 0; i < num_threads_gp; i++) begin
        if (monitor_cam[i].valid &&
            (dcache_write_addr == monitor_cam[i].address[63:6])) begin
            // Monitored address was written - mark thread as waking
            wakeup_thread[i] = 1'b1;
        end
    end
end
```

2. **Create monitor address register in CSRs**:
   - Already defined: `CSR_MONITOR_ADDR` (0xC05)
   - Write to this CSR adds entry to monitor CAM

```systemverilog
// In CSR write handler
if (csr_waddr == CSR_MONITOR_ADDR) begin
    monitor_cam[active_tid].address = csr_write_data;
    monitor_cam[active_tid].valid = 1'b1;
end
```

3. **Also monitor external writes** (I/O, DMA):
   - Listen to L2 cache write-backs
   - Listen to interconnect for remote writes
   - Challenge: Requires coherent interconnect support

**Why Difficult**:
- Monitor must work for ANY write (DMA, NIC, other cores)
- Current interconnect doesn't track which core is watching
- Solution: Extend coherence protocol or use L1 cache snoop

**Timeline**: Week 14-15

**Validation**:
- ✅ Monitor CAM stores correct address
- ✅ Thread wakes when address written by CPU
- ✅ Thread wakes when address written by other core
- ⚠️ DMA wakeup requires interconnect extension

---

### Step 4.2: mwait Instruction and Thread Blocking

**What**: Instruction to block thread until monitored address is written

**ISA Format**:
```
monitor <mem_addr>
    # Set up monitoring

mwait
    # Block until monitored address modified
    # When address is written, thread becomes runnable
```

**Example Usage**:
```asm
# Kernel: Wait for I/O interrupt
li x1, 0x1000        # I/O event descriptor address
monitor x1           # Monitor that address
li x2, 0             # Clear x2 (for loop)
loop:
    addi x2, x2, 1   # Do work while waiting
    bne x2, x100, loop
mwait                # Block here until address written
# When NIC writes packet descriptor to 0x1000, thread wakes
```

**Implementation**:

1. **Add monitor instruction** (CSR write variant):
   - Monitor is actually a CSR write: `csrw csr_monitor_addr, rs1`
   - Sets `CSR_MONITOR_ADDR` and enables the CAM entry

2. **Add mwait instruction**:

```systemverilog
// In instruction decoder
case (instr[6:0])
    7'b1110011: begin  // CSR opcode
        case (instr[31:25])
            7'b0001000: begin  // mwait
                pipe_sel = MWAIT_PIPE;
                instr_type = MWAIT;
            end
        endcase
    end
endcase
```

3. **Create mwait execution unit** (`bp_be_mwait.sv`):

```systemverilog
module bp_be_mwait (
    input clk_i,
    input reset_i,

    // From dispatch
    input bp_be_dispatch_packet_s dispatch_i,

    // Monitor CAM
    input logic [63:0] monitored_addr_i,
    input logic monitor_valid_i,

    // Thread state
    output logic thread_block_o,      // Block this thread
    input logic monitor_wakeup_i,     // Address was written

    // To writeback
    output bp_be_writeback_packet_s writeback_o
);

    always @(posedge clk_i) begin
        if (dispatch_i.instr_type == MWAIT) begin
            if (!monitor_valid_i) begin
                // No monitored address - exception
                writeback_o.exception_valid = 1'b1;
                writeback_o.exception_code = ILLEGAL_INSTRUCTION;
            end else begin
                // Block thread until wakeup
                thread_block_o = 1'b1;
            end
        end
    end

    // On wakeup, scheduler will resume thread
endmodule
```

4. **Update scheduler to handle blocked threads**:
   - Don't issue instructions from blocked threads
   - When wakeup signal asserts, unblock thread
   - Resume from instruction after mwait

**Timeline**: Week 15-16

**Validation**:
- ✅ monitor sets address in CAM
- ✅ mwait blocks thread
- ✅ Thread unblocks when address written
- ✅ Performance: No polling, true blocking

**Success Criteria**:
- ✅ Thread blocks on mwait
- ✅ Thread wakes on address write
- ✅ Can monitor L1 cache writes
- ⚠️ DMA/I/O wakeup requires more work (Phase 5)

---

## Phase 5: Exception Handling (Weeks 17-20)

### Objectives
- Disable thread on exception (instead of context switch)
- Write exception descriptor to memory
- Let separate exception handler thread manage exceptions
- Eliminate context switching cost

### Critical Innovation ⚠️
This is the **most impactful** change - removing all interrupts and context switches

---

### Step 5.1: Exception Descriptor Storage

**What**: When an exception occurs, write descriptor to memory instead of jumping

**Current Behavior** (single-threaded):
```
Exception occurs →
  Save registers →
  Jump to exception handler →
  Handler reads saved state
```

**New Behavior** (multi-threaded):
```
Exception occurs →
  Write descriptor to memory →
  Disable current thread →
  Scheduler picks exception handler thread
  (Handler reads descriptor from memory)
```

**Implementation**:

1. **Define exception descriptor struct**:

```systemverilog
typedef struct packed {
    logic [7:0]  thread_id;        // Which thread faulted
    logic [7:0]  exception_code;   // Exception type
    logic [63:0] faulting_addr;    // For TLB/memory exceptions
    logic [63:0] pc;               // PC where exception occurred
    logic [63:0] npc;              // Next PC
    logic [63:0] registers[31:0];  // Dumped register state (optional)
} exception_descriptor_s;           // 280+ bytes
```

2. **Update exception handler in `bp_be_writeback.sv`**:

```systemverilog
// When exception detected
if (exception_valid) begin
    // Instead of jumping to exception handler:

    // 1. Get exception descriptor address from context
    desc_addr = exception_ptr_from_context[active_tid];

    // 2. Write descriptor to memory (burst DMA-like write)
    dma_write_req.valid = 1'b1;
    dma_write_req.addr = desc_addr;
    dma_write_req.data = pack(exception_descriptor);
    dma_write_req.size = 64;  // Write full descriptor

    // 3. Disable current thread
    thread_disabled[active_tid] = 1'b1;

    // 4. Let scheduler pick exception handler thread
    // (Handler thread is monitoring exception_ptr address)
end
```

3. **Create DMA-like memory interface** for descriptor writes:
   - Doesn't stall pipeline (write happens in background)
   - Uses L1 D-cache or special write buffer
   - Coordinates with coherence protocol

**Location**: Modify `bp_be_writeback.sv` and related modules

**Timeline**: Week 17-18

**Validation**:
- ✅ Exception descriptor correctly written
- ✅ Thread disabled
- ✅ Next exception handler thread scheduled
- ✅ No context-switch latency

---

### Step 5.2: Exception Handler Thread Setup

**What**: Configure dedicated thread to handle exceptions

**How** (software side):

```asm
# Kernel initialization
# Thread 0: Main application
# Thread 1: Exception handler

# In exception handler thread:
exception_handler:
    li x1, exception_desc_addr
    monitor x1                    # Wait for exceptions
loop:
    mwait                         # Block until exception

    # Exception descriptor now in memory at x1
    ld x2, 0(x1)                  # Read exception_code

    if (x2 == PAGE_FAULT) then
        # Handle page fault
        # Update page table
        # Start faulting thread again

    if (x2 == SYSCALL) then
        # Handle syscall
        # Return result

    j loop
```

**How** (hardware side):

1. **Define CSR for exception descriptor pointer**:
   - Already defined: `CSR_EXCEPTION_PTR` (0xC03)
   - Each thread sets own exception descriptor address
   - Can be in thread's private memory or shared region

2. **Monitor in exception handler**:
   - Exception handler calls `monitor exception_desc_addr`
   - When exception occurs in any thread, kernel writes to that address
   - Exception handler thread wakes up

3. **Privilege requirement**:
   - Only supervisor mode can read/write exception descriptors
   - Prevents unprivileged code from seeing other threads' faults

**Timeline**: Week 18

**Success Criteria**:
- ✅ Exception descriptor written to correct address
- ✅ Exception handler thread wakes on exception
- ✅ Faulting thread disabled until handled
- ✅ No interrupt dispatch latency

---

### Step 5.3: Nested Exception Handling

**What**: Handle exceptions that occur while handling exceptions

**Problem**: What if exception handler itself faults?

**Solution** (from paper): Exception chains ending in catch-all thread

**Architecture**:
```
User Thread 0: Faults → writes to handler_addr[0]
Exception Handler Thread 1: Faults → writes to handler_addr[1]
Fatal Exception Handler Thread 2: Catches everything

If Thread 2 faults → CPU halt (triple fault)
```

**Implementation**:

1. **Each thread can have exception handler pointer**:
   - `exception_ptr_for_thread[tid]` = where to write exceptions for tid
   - Forms a chain: Thread 0 → Handler 1 → Handler 2 → Halt

2. **Update exception dispatch**:

```systemverilog
// When exception in thread X:
if (exception_handler_thread[X] exists) begin
    write_exception_descriptor(exception_ptr_for_thread[X]);
    disable_thread(X);
    thread_enabled[exception_handler[X]] = 1'b1;  // Wake handler
end else begin
    // No handler - halt
    cpu_halt = 1'b1;
end
```

3. **Initialize TDT with exception chains**:
```
Thread 0: exception_ptr → 0x1000, handler=1
Thread 1: exception_ptr → 0x2000, handler=2
Thread 2: exception_ptr → 0x3000, handler=none (halt on fault)
```

**Timeline**: Week 19

**Validation**:
- ✅ Thread exception triggers correct handler
- ✅ Handler exception triggers next handler
- ✅ Fatal exception halts CPU

---

### Step 5.4: Page Fault Handling Without Context Switch

**What**: Implement classic page fault handling without stopping execution of other threads

**Scenario**:
```
Thread 0: Load from unmapped address
  → Page fault exception
  → Write descriptor to memory at 0x2000
  → Thread 0 disabled

Handler Thread: Monitoring 0x2000
  → Wakes up, reads fault descriptor
  → Allocates physical page
  → Updates page tables / TLB
  → rpush updates Thread 0's page table pointer
  → start enables Thread 0 again

Other threads: Keep executing (never interrupted!)
```

**Implementation**:

1. **Fault descriptor includes faulting address**:
```systemverilog
exception_descriptor.faulting_addr = fault_addr;  // VA that caused fault
```

2. **Handler reads and processes**:
```asm
# In handler thread
ld x1, fault_addr_offset(x2)   # Get faulting VA
# ... allocate page, update tables ...
# Resume user thread
rpush x0, x10, x11              # Write page table pointer
start x0                        # Resume user thread
```

3. **No TLB/cache flush needed**:
   - Updated page tables take effect immediately when thread resumes
   - Other threads unaware of the fault (no IPI)

**Timeline**: Week 19-20

**Success Criteria**:
- ✅ Page fault doesn't interrupt other threads
- ✅ Handler can allocate and setup page
- ✅ User thread resumes transparently
- ✅ Performance: No cache/TLB thrashing

---

## Phase 6: Thread Scheduling (Weeks 21-23)

### Objectives
- Implement round-robin scheduling of runnable threads
- Fair thread switching (prevent starvation)
- Priority support for critical threads
- Hardware efficiency (don't run empty threads)

---

### Step 6.1: Hardware Round-Robin Scheduler

**What**: Scheduler that switches between runnable threads fairly

**Current State** (single-threaded):
- Fetch one instruction per cycle from single thread

**New State** (multi-threaded):
- Rotate which thread executes each cycle
- If thread is blocked (mwait) or disabled, skip it

**Implementation** in `bp_be_scheduler.sv`:

```systemverilog
module bp_be_scheduler_mt (
    input clk_i,
    input reset_i,

    // Thread state
    input [num_threads_gp-1:0] thread_enabled_i,
    input [num_threads_gp-1:0] thread_blocked_i,  // Waiting on mwait

    // Scheduler output
    output [7:0] active_tid_o,      // Which thread to execute
    output [63:0] pc_o,             // Its PC

    // Issue queue
    input bp_be_issue_queue_s issue_q_i [num_threads_gp-1:0],
    output bp_be_dispatch_packet_s dispatch_o
);

    // Current position in round-robin
    logic [7:0] current_tid_r;

    // Per-thread instruction counters (for fairness)
    logic [15:0] instr_count_r [num_threads_gp-1:0];
    parameter MAX_CONSECUTIVE = 8;  // Max instructions before switching

    always @(posedge clk_i) begin
        // Find next runnable thread
        logic [7:0] next_tid;
        next_tid = current_tid_r;

        // Rotate until we find a runnable thread
        for (int i = 0; i < num_threads_gp; i++) begin
            next_tid = (next_tid + 1) % num_threads_gp;
            if (thread_enabled_i[next_tid] && !thread_blocked_i[next_tid]) begin
                break;
            end
        end

        // Switch if we've issued max instructions from current thread
        if (instr_count_r[current_tid_r] >= MAX_CONSECUTIVE) begin
            current_tid_r <= next_tid;
            instr_count_r[current_tid_r] <= 0;
        end else if (dispatch_o.valid && dispatch_o.thread_id == current_tid_r) begin
            instr_count_r[current_tid_r] <= instr_count_r[current_tid_r] + 1;
        end
    end

    // Route instructions from current thread
    assign active_tid_o = current_tid_r;
    assign dispatch_o = issue_q_i[current_tid_r];

endmodule
```

**Key Points**:
- Simple rotation: 0 → 1 → 2 → ... → N → 0
- Skip disabled/blocked threads
- Fairness: Switch every ~8 instructions (prevents starvation)
- No scheduler overhead (combinational logic)

**Timeline**: Week 21

**Validation**:
- ✅ All runnable threads execute
- ✅ Blocked threads skipped
- ✅ Fair switching (no starvation)
- ✅ No IPC degradation for one thread

---

### Step 6.2: Priority-Based Scheduling

**What**: Allow some threads (e.g., interrupt handlers) to execute more often

**Mechanism**: Priority bits in thread status register

**Implementation**:

```systemverilog
// Thread priority levels
typedef enum {
    PRIORITY_LOW = 2'b00,       // Background work
    PRIORITY_NORMAL = 2'b01,    // Default
    PRIORITY_HIGH = 2'b10,      // I/O handlers
    PRIORITY_CRITICAL = 2'b11   // Real-time
} thread_priority_e;

// In scheduler
always @(*) begin
    // Find highest-priority runnable thread
    logic [7:0] next_tid_by_priority;
    logic [1:0] best_priority = 2'b00;

    for (int i = 0; i < num_threads_gp; i++) begin
        if (thread_enabled_i[i] && !thread_blocked_i[i]) begin
            if (thread_priority_i[i] > best_priority) begin
                best_priority = thread_priority_i[i];
                next_tid_by_priority = i;
            end
        end
    end

    // Switch to higher-priority thread immediately
    if (thread_priority_i[next_tid_by_priority] >
        thread_priority_i[current_tid_r]) begin
        current_tid_r <= next_tid_by_priority;
    end
end
```

**Use Cases**:
- Timer interrupt handler: CRITICAL priority
- Network RX handler: HIGH priority
- User application: NORMAL priority
- Background work: LOW priority

**Timeline**: Week 22

**Success Criteria**:
- ✅ High-priority threads run more often
- ✅ Critical handlers get CPU time
- ✅ Still fair to lower-priority threads

---

### Step 6.3: Measuring Thread Resource Consumption

**What**: Track how much CPU each thread has used

**Why**: Important for cloud billing, scheduling decisions, and debugging

**Implementation**:

```systemverilog
// Per-thread cycle counter (for billing)
logic [63:0] thread_cycles_r [num_threads_gp-1:0];

always @(posedge clk_i) begin
    // Increment counter for active thread
    if (thread_enabled_i[active_tid_o] && !thread_blocked_i[active_tid_o]) begin
        thread_cycles_r[active_tid_o] <= thread_cycles_r[active_tid_o] + 1;
    end
end

// CSR to read cycle counter
assign csr_read_data = thread_cycles_r[active_tid_o];  // CSR_THREAD_CYCLES
```

**Timeline**: Week 23

**Success Criteria**:
- ✅ Can read per-thread cycle counters
- ✅ Used for billing
- ✅ Kernel can make scheduling decisions

---

## Phase 7: ISA Extensions (Weeks 24-26)

### Objectives
- Add invtid instruction (TDT invalidation)
- Verify all new instructions compile correctly
- Create instruction reference documentation
- Ensure privilege checks are enforced

---

### Step 7.1: Implement invtid (TDT Invalidation)

**What**: Instruction to flush cached TDT entries

**Why Needed**:
- Kernel may change TDT in memory to change permissions
- Hardware may cache TDT entries for performance
- invtid tells hardware to invalidate cache

**ISA Format**:
```
invtid <vtid>, <remote_vtid>
    Invalidate cached TDT entry for <remote_vtid> in context of <vtid>
```

**Implementation**:

```systemverilog
// Add to instruction decoder
case (instr[6:0])
    7'b0001101: begin  // invtid opcode
        instr_type = INVTID;
        pipe_sel = THREAD_CONTROL_PIPE;
    end
endcase

// In thread control unit
if (dispatch_i.instr_type == INVTID) begin
    // Invalidate cached permissions for target thread
    tdt_cache_valid[vtid] <= 1'b0;    // Mark cache entry invalid
    // Next start/stop will re-read from memory
end
```

**Timeline**: Week 24

**Validation**:
- ✅ TDT cache invalidated
- ✅ Next access re-reads from memory
- ✅ Permission changes take effect

---

### Step 7.2: Complete ISA Reference

**What**: Document all new instructions in RISC-V ISA style

**Format**:

| Instruction | Encoding | Format | Privilege | Purpose |
|------------|----------|--------|-----------|---------|
| start      | custom-0 (funct7=0x00) | R-type | S | Enable thread |
| stop       | custom-0 (funct7=0x01) | R-type | S | Disable thread |
| rpull      | custom-1 (funct7=0x00) | R-type | S | Read remote reg |
| rpush      | custom-1 (funct7=0x01) | R-type | S | Write remote reg |
| monitor    | CSR write (0xC05)      | I-type | U | Set monitor addr |
| mwait      | custom-2 (funct7=0x00) | I-type | U | Block on monitor |
| invtid     | custom-3 (funct7=0x00) | R-type | S | Invalidate TDT |

**Create Documentation**:
- New file: `black-parrot/docs/threading_isa_extensions.md`
- Include: Encoding, semantics, privilege requirements, examples

**Timeline**: Week 25

**Validation**:
- ✅ No encoding conflicts with RISC-V spec
- ✅ No conflicts with future extensions
- ✅ Documentation complete and accurate

---

### Step 7.3: Privilege and Exception Verification

**What**: Comprehensive validation of all privilege and exception paths

**Test Cases**:
1. User mode cannot execute privileged instructions
2. Unprivileged start/stop/rpull/rpush raises exception
3. Permission bits in TDT are enforced
4. Exception handlers correctly isolate failures
5. Nested exceptions handled correctly

**Test Suite**:

```asm
# test_privilege.s
test_user_start:
    # User mode thread tries to start another thread
    li x1, 1
    start x1              # Should raise PRIVILEGE_FAULT
    j test_failed

test_permission_denied:
    # Thread 0 tries to start Thread 1 without permission
    # (TDT[1].permissions = 0)
    li x1, 1
    start x1              # Should raise PERMISSION_FAULT
    j test_failed

test_supervisor_allowed:
    # Switch to supervisor mode
    li x1, SUPERVISOR_MODE
    csrw mstatus, x1      # Set privilege level
    li x2, 1
    start x2              # Should succeed
    j test_passed
```

**Timeline**: Week 25-26

**Success Criteria**:
- ✅ All privilege exceptions raised correctly
- ✅ All permission checks enforced
- ✅ Nested exceptions handled correctly
- ✅ No security bypasses

---

## Phase 8: Integration and Testing (Weeks 27-32)

### Objectives
- Full system integration
- Comprehensive testing
- Performance characterization
- Documentation and handoff

---

### Step 8.1: Full System Integration

**What**: Integrate all components (regfile, scheduler, exceptions, monitor/mwait)

**Integration Checklist**:
```
Core Components:
  ☐ Expanded register file (8K entries)
  ☐ Thread context storage (distributed FFs)
  ☐ Thread ID routing through pipeline
  ☐ Thread control unit (start/stop)
  ☐ Register access unit (rpull/rpush)
  ☐ Monitor CAM and mwait logic
  ☐ Exception handler (write descriptors)
  ☐ Round-robin scheduler
  ☐ Priority scheduler
  ☐ CSRs for thread management

Control Paths:
  ☐ start enables thread
  ☐ stop disables thread
  ☐ mwait blocks thread
  ☐ Monitor wakeup unblocks thread
  ☐ Exception disables and signals handler
  ☐ Scheduler rotates between runnable threads

Data Paths:
  ☐ Register writes go to correct thread
  ☐ Register reads fetch from correct thread
  ☐ rpull/rpush access remote registers
  ☐ Exception descriptors written to memory
  ☐ Thread contexts saved/restored correctly
```

**Timeline**: Week 27-28

**Validation**:
- ✅ Simulation with 16 threads passes
- ✅ No data corruption between threads
- ✅ No deadlocks or hangs
- ✅ Performance metrics collected

---

### Step 8.2: Comprehensive Testing Suite

**What**: Tests covering all threading features

**Test Categories**:

1. **Single Thread Tests** (baseline):
   - Execute single thread (backward compatibility)
   - Verify same behavior as original Black Parrot
   - No performance regression

2. **Multi-Thread Basic**:
   - Start/stop multiple threads
   - Threads execute independently
   - Register isolation verified

3. **Context Switching**:
   - Start thread, let run 100 instructions
   - Stop thread, read state with rpull
   - Modify state with rpush
   - Resume thread
   - Verify modified state in use

4. **Exception Handling**:
   - Thread 0 takes page fault
   - Handler thread wakes
   - Handler reads exception descriptor
   - Handler updates page table
   - Thread 0 resumes
   - No other threads interrupted

5. **Monitor/mwait**:
   - Thread blocks on mwait (monitored address)
   - Other threads continue
   - Address written by another thread
   - Waiting thread wakes
   - Resumes from after mwait

6. **Nested Exceptions**:
   - Thread 0 faults
   - Handler (Thread 1) faults
   - Final handler (Thread 2) handles
   - Isolation verified

7. **Performance**:
   - Measure IPC with 1 thread (baseline)
   - Measure IPC with 16 threads (should be better utilization)
   - Measure context switch overhead (should be negligible)
   - Measure exception handling latency (should be nanoseconds)

**Timeline**: Week 28-29

**Success Criteria**:
- ✅ 100+ test cases pass
- ✅ No data corruption
- ✅ Performance meets expectations
- ✅ All features work together correctly

---

### Step 8.3: FPGA Validation

**What**: Deploy to FPGA board (if available) to verify in real hardware

**Target**: Can provide more accurate performance and power measurements

**Steps**:
1. Synthesize for target FPGA (e.g., Xilinx VCU1525)
2. Boot Linux kernel with threading support
3. Run real workloads (I/O heavy, page faults, interrupts)
4. Measure latency, throughput, power
5. Compare to baseline single-threaded

**Timeline**: Week 29-30

**Success Criteria**:
- ✅ Synthesizes and boots on FPGA
- ✅ Real workloads run correctly
- ✅ Performance improvements measured
- ✅ No hardware bugs discovered

---

### Step 8.4: Documentation and Knowledge Transfer

**What**: Complete documentation for team and future developers

**Deliverables**:

1. **Architecture Guide** (`docs/threading_architecture.md`):
   - Overview of threading model
   - Comparison to context switching
   - Performance characteristics
   - Hardware cost analysis

2. **Implementation Guide** (`docs/threading_implementation.md`):
   - How each phase was implemented
   - Key design decisions
   - Integration points
   - Known limitations and future work

3. **ISA Reference** (`docs/threading_isa_extensions.md`):
   - New instructions (start, stop, rpull, rpush, monitor, mwait, invtid)
   - CSRs (PTID, VTID, THREAD_STAT, EXCEPTION_PTR, TDT_PTR, etc.)
   - Privilege requirements
   - Examples

4. **Software Developer Guide** (`docs/threading_software_guide.md`):
   - How to write multi-threaded software
   - Thread creation and management
   - Exception handling
   - I/O patterns (no polling)
   - Performance tuning

5. **Test Report** (`test/threading_test_report.md`):
   - Test coverage
   - Results
   - Performance benchmarks
   - Issues found and fixed

**Timeline**: Week 30-31

**Deliverables**:
- ✅ 50+ pages of documentation
- ✅ Code examples
- ✅ Performance graphs
- ✅ Known issues list

---

### Step 8.5: Kernel Integration

**What**: Integrate threading support into Linux kernel (optional, Phase 8+)

**Software Needed**:
1. Device driver for thread management
2. New system calls for thread control
3. Kernel scheduler integration
4. I/O device drivers updated for monitor/mwait

**Timeline**: Beyond Phase 8 (advanced work)

---

### Step 8.6: Final Validation and Release

**What**: Final testing and readiness assessment

**Release Checklist**:
```
Testing:
  ☐ All unit tests pass
  ☐ Integration tests pass
  ☐ Stress tests pass (1000s of context switches)
  ☐ Regression tests pass (existing code unaffected)
  ☐ FPGA validation complete

Quality:
  ☐ Code review completed
  ☐ No critical bugs open
  ☐ Documentation complete
  ☐ Known issues documented

Performance:
  ☐ Timing closure met
  ☐ Performance targets met
  ☐ Power within budget

Safety:
  ☐ Privilege checks enforced
  ☐ No security bypasses
  ☐ Isolation between threads verified
```

**Timeline**: Week 31-32

**Success Criteria**:
- ✅ All checkboxes complete
- ✅ Ready for production use
- ✅ Team trained
- ✅ Documentation available

---

## Summary: Incremental Progress Milestones

### End of Phase 0 (Week 2)
**Output**: Design documents, test harness
**Capability**: Can test individual features
**Risk**: Low (design only)

### End of Phase 1 (Week 6)
**Output**: Working expanded register file
**Capability**: Multiple threads can store state
**Risk**: High (register file timing)
**Mitigation**: Early prototype and synthesis

### End of Phase 2 (Week 10)
**Output**: start/stop instructions, thread enable/disable
**Capability**: Software can manage thread execution
**Risk**: Medium (privilege checks must be correct)
**Mitigation**: Comprehensive privilege testing

### End of Phase 3 (Week 13)
**Output**: rpull/rpush instructions, context access
**Capability**: Can swap thread state without memory operations
**Risk**: Medium (data corruption if incorrect)
**Mitigation**: Exhaustive context swap tests

### End of Phase 4 (Week 16)
**Output**: monitor/mwait, address tracking
**Capability**: Threads can block on addresses (true I/O waiting)
**Risk**: Medium (requires interconnect integration)
**Mitigation**: Start with L1-only monitoring, extend later

### End of Phase 5 (Week 20)
**Output**: Exception handling, descriptor writing
**Capability**: Exceptions don't interrupt other threads
**Risk**: High (exception nesting is complex)
**Mitigation**: Careful handler chain design, extensive testing

### End of Phase 6 (Week 23)
**Output**: Hardware scheduler, round-robin, priorities
**Capability**: Fair scheduling of multiple threads
**Risk**: Low (straightforward logic)
**Mitigation**: Fairness proofs, starvation tests

### End of Phase 7 (Week 26)
**Output**: ISA extensions complete, instruction set stable
**Capability**: Software fully can control threads
**Risk**: Low (ISA design is final)
**Mitigation**: Compatibility testing with existing code

### End of Phase 8 (Week 32)
**Output**: Fully integrated, tested, documented system
**Capability**: Production-ready multi-threaded processor
**Risk**: Low (comprehensive testing)
**Status**: Ready for deployment

---

## Resource Allocation

**Recommended Team** (4-6 people, 32 weeks):
- **Lead Architect** (1 person, 32 weeks): Overall design, reviews
- **Hardware Engineer 1** (1 person, 32 weeks): Register file, pipeline integration
- **Hardware Engineer 2** (1 person, 32 weeks): Scheduler, exception handling
- **Hardware Engineer 3** (1 person, Weeks 14-32): Monitor/mwait, integration
- **Software Engineer** (1 person, Weeks 17-32): Testing, validation, documentation
- **CAD/Verification Engineer** (0.5 person, Weeks 3-6, 27-32): Synthesis, FPGA

**Parallel Work**:
- Phases 1-3: Core hardware engineers can work in parallel
- Phases 4-6: Independent features (scheduler, exception handling)
- Phases 7-8: Everything converges to integration

**Cost Estimate**:
- Salaries: 6 people × 8 weeks average = 48 person-weeks
- Silicon area: ~20% increase (manageable)
- Power overhead: ~15-25% (from larger SRAM)
- Schedule risk: ~20% (register file timing might slip)

---

## Key Success Factors

1. **Register File Timing**: Spend weeks 3-6 getting this right
2. **Comprehensive Testing**: Don't skip any test case
3. **Privilege Isolation**: Security review required
4. **Backward Compatibility**: Single-threaded code must still work
5. **Documentation**: Write as you go, not at the end
6. **Team Communication**: Daily standups, weekly design reviews

---

## Next Steps

**Now**:
1. Read this plan thoroughly
2. Review Phase 0 design documents
3. Setup baseline Black Parrot build

**Week 1**:
1. Team kickoff meeting
2. Assign engineers to phases
3. Create detailed Phase 0 designs
4. Start Phase 0 work

**Week 2**:
1. Complete Phase 0
2. Setup testing infrastructure
3. Begin Phase 1 (register file design)
4. Schedule synthesis for Week 4

**Weeks 3-32**: Execute phases as planned, with weekly reviews

---

## Appendix A: File Changes Summary

### New Files to Create
- `black-parrot/bp_common/src/v/bp_common_thread_state.svh` (thread context struct)
- `black-parrot/bp_be/src/v/bp_be_checker/bp_be_regfile_mt.sv` (expanded regfile)
- `black-parrot/bp_be/src/v/bp_be_checker/bp_be_thread_ctx_storage.sv` (context storage)
- `black-parrot/bp_be/src/v/bp_be_checker/bp_be_thread_control.sv` (start/stop logic)
- `black-parrot/bp_be/src/v/bp_be_checker/bp_be_regfile_access.sv` (rpull/rpush)
- `black-parrot/bp_be/src/v/bp_be_checker/bp_be_mwait.sv` (monitor/mwait)
- `black-parrot/bp_be/src/v/bp_be_checker/bp_be_scheduler_mt.sv` (multi-thread scheduler)
- `docs/threading_isa_extensions.md` (ISA reference)
- `docs/threading_architecture.md` (design overview)
- `docs/threading_implementation.md` (implementation guide)

### Files to Modify
- `bp_common_rv64_pkgdef.svh` (add new CSRs)
- `bp_common_aviary_cfg_pkgdef.svh` (add threading config)
- `bp_common_aviary_defines.svh` (add thread sizing macros)
- `bp_be_instr_decoder.sv` (decode new instructions)
- `bp_be_scheduler.sv` (thread control integration)
- `bp_be_writeback.sv` (exception handling)
- `bp_fe_pc_gen.sv` (thread-aware PC)
- `bp_be_top.sv` (instantiate new modules)

### Files to Extend
- `bp_be_csr.sv` (handle new CSRs)
- `bp_be_dcache.sv` (monitor address tracking)
- Configuration system (thread count parameters)

---

**Total Implementation Effort**: 32 weeks | 4-6 people | ~$1.5M-2M in salaries | ~20% area overhead

**Expected Result**: Production-ready multi-threaded Black Parrot processor supporting elimination of context switching, enabling faster I/O, exceptions, and system calls.

