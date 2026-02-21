# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Repository Overview

This is a meta-repository that packages the BlackParrot RISC-V processor RTL, Software Development Kit (SDK), and simulation tools into a unified environment. The primary use case is hardware simulation and evaluation of the BlackParrot multicore processor.

Three main submodules:
- `black-parrot/` — RTL and simulation environment (forked at `connoryang1/black-parrot`)
- `black-parrot-sdk/` — Software tools, test programs, compilers, and RISC-V toolchain
- `black-parrot-tools/` — Open-source simulation/synthesis tools (Verilator, Yosys, etc.)

## Environment Variables

Defined in `Makefile.env`:
- `BP_DIR` — repo root
- `BP_INSTALL_DIR` — built toolchains (`install/`)
- `BP_RISCV_DIR` — RISC-V binaries (`riscv/`)
- `BP_WORK_DIR` — intermediate build artifacts (`work/`)

## Key Commands

### Setup
```bash
make checkout          # Initialize and checkout all submodules
make prep_lite         # Minimal setup (Verilator + basic toolchain + lite programs)
make prep              # Full setup (includes prep_lite + full toolchain + all programs)
make prep_bsg          # Additional setup for BSG users (CAD environment)
```

### Running Tests
```bash
# Run the default hello_world test with Verilator
make -C black-parrot/bp_top/verilator build.verilator sim.verilator

# Run a specific test program
make -C black-parrot/bp_top/verilator sim.verilator SUITE=bp-tests PROG=<test_name>

# Force rebuild the simulator then run a test
make -C black-parrot/bp_top/verilator build.verilator -B
make -C black-parrot/bp_top/verilator sim.verilator SUITE=bp-tests PROG=<test_name>
```

Common test variables:
- `SUITE` — test suite (default: `bp-tests`)
- `PROG` — specific program name
- `TB` — testbench (default: `bp_tethered`)
- `CFG` — configuration (default: `e_bp_default_cfg`)
- `TRACE=1` — enable waveform tracing
- `DROMAJO_COSIM=1` / `SPIKE_COSIM=1` — enable co-simulation
- `COMMITLOG=1` — log committed instructions

### Custom Test (multithreading_demo)
```bash
# Compile and run the multithreading_demo test from scratch
./testing/rebuild-and-test.sh
```

To compile a test manually using the installed toolchain:
```bash
install/bin/riscv64-unknown-elf-gcc \
  -o black-parrot-sdk/riscv/bp-tests/<test>.riscv \
  testing/<test>.c \
  -I black-parrot-sdk/libperch \
  -march=rv64gc -mabi=lp64d \
  --specs=install/riscv64-unknown-elf/lib/dramfs.specs \
  --specs=install/riscv64-unknown-elf/lib/perch.specs
```

### Maintenance
```bash
make clean             # Clean submodule working directories
make tidy              # Unpatch submodules
make bleach            # Deinitialize submodules
make bleach_all        # Complete wipe (use with caution)
make help              # List all documented targets
```

### Docker
```bash
make -C docker docker-image DOCKER_PLATFORM=ubuntu24.04
make -C docker docker-run DOCKER_PLATFORM=ubuntu24.04
# Add USE_LOCAL_CREDENTIALS=1 to mount with local uid/gid
```

## Architecture

### RTL Pipeline (`black-parrot/`)
- `bp_fe/` — Front-end: instruction fetch and decode
- `bp_be/` — Back-end: execute, load/store, writeback
- `bp_me/` — Memory system: distributed L2 cache and BedRock coherence protocol
- `bp_common/` — Shared packages, CSR definitions, interface specifications
- `bp_top/` — Top-level integration + testbenches (`bp_top/test/tb/bp_tethered/`)
- `bp_top/verilator/` — Verilator simulation harness (primary simulation target)

### Test Programs (`black-parrot-sdk/riscv/bp-tests/`)
60+ compiled `.riscv` binaries covering: instruction set, cache coherence, exceptions, interrupts, floating point, multicore AMO/LR-SC, paging, and virtual memory.

Test sources live in `black-parrot-sdk/bp-tests/`.

### Custom Tests (`testing/`)
Test programs added to this repo (not the SDK) for development work. After compiling, place the binary in `black-parrot-sdk/riscv/bp-tests/`.

## Working Practices

### Subagent Usage
Use subagents proactively to preserve context window:
- **Explore agent** — broad codebase exploration, understanding unfamiliar subsystems, tracing data/signal flow across multiple files
- **Bash agent** — running builds, tests, or multi-step shell sequences
- **Plan agent** — designing changes before touching RTL or SDK files
- Launch multiple independent subagents in parallel when tasks don't depend on each other

### Mistake Tracking
When a mistake is made or an issue is fixed, update this CLAUDE.md immediately so the same error doesn't recur.

## Active Development

This fork is implementing **hardware thread management** to support software-managed many-threading without context-switch overhead (based on "A Case Against (Most) Context Switches", HotOS 2021).

Implementation plan: `docs/INCREMENTAL_IMPLEMENTATION_PLAN.md`

`prev.patch` is from an older BlackParrot version — do NOT apply it. It is kept only as a reference.

### What is committed (commit `f12b9616`)

New RTL files in `black-parrot/bp_be/src/v/`:
- `bp_be_thread_scheduler.sv` — round-robin scheduler with CSR `0x081` override
- `bp_be_context_storage.sv` — per-thread NPC/priv/ASID state storage
- `bp_be_csr_wrapper_mt.sv`, `bp_be_regfile_mt.sv`, `bp_be_scheduler_mt.sv` — multithreaded variants
- `bp_common/src/v/bp_common_thread_state.svh` — thread state struct definition

Modified files:
- `bp_be/src/v/bp_be_calculator/bp_be_csr.sv` — CSR `0x081` read/write (CTXT, current thread ID)
- `bp_be/src/v/bp_be_top.sv` — wires up scheduler and context storage
- `bp_common/src/include/bp_common_aviary_cfg_pkgdef.svh` — added `num_threads`, `thread_id_width` params

### What is NOT yet implemented

- Phase 0 thread management CSRs (`bp_be_csr.sv` has no handlers for them yet)

### Critical: CSR address conflict to fix

**`0xC00`–`0xC1F` are fully occupied by standard RISC-V user-mode performance counters** (`cycle`, `time`, `instret`, `hpmcounterN`) — do NOT use this range for custom thread CSRs.

The correct range for custom thread management CSRs is **`0x5xx`** (supervisor custom-use space). The stash `stash@{0}` uses `0x580`–`0x58C`. `bp_common_thread_state.svh` currently has the wrong addresses (`0xC00`–`0xC0B`) and must be updated when Phase 0 CSRs are implemented.

### `bp_utils.h` / `bp_utils.c` (in `black-parrot-sdk/libperch/`)

This is the bare-metal runtime library for test programs. It provides character I/O via memory-mapped MMIO and `bp_finish`/`bp_panic`. **Do not modify this repo** — CSR accessors are inlined directly in the test using `__asm__ volatile("csrr ...")`. Use `bp_hprint_uint64` (hex, already exists) for printing numbers — `bp_print_uint64` (decimal) does not exist and we are not adding it.

### Test strategy

Test only what is actually implemented in RTL. Do not test Phase 0 CSRs until their RTL handlers exist. Each phase of RTL work should have a corresponding minimal test that can be verified to 100% certainty before moving to the next phase.

`testing/multithreading_demo.c` is currently Phase 1.4 only (CSR `0x081` context switching) and passes `[BSG-PASS]`. Verified results: context switches 0→3 all correct, sum=0x1356 (4950), fib=0x262 (610).
