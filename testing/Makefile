# testing/Makefile
#
# Compile and run BlackParrot custom tests against the Verilator simulator.
#
# Usage:
#   make run-<test>    compile (if stale) and run a single test
#   make all           run all tests (no simulator rebuild)
#   make sim           rebuild the Verilator simulator from RTL (expensive)
#   make clean         delete compiled test binaries
#   make help          list available tests and targets
#
# Examples:
#   make run-mt_frf_isolation_test
#   make run-mt_benchmark
#   make sim && make all

ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))..)

CC     := $(ROOT)/install/bin/riscv64-unknown-elf-gcc
CFLAGS := -I$(ROOT)/black-parrot-sdk/libperch \
           -march=rv64gc -mabi=lp64d \
           --specs=$(ROOT)/install/riscv64-unknown-elf/lib/dramfs.specs \
           --specs=$(ROOT)/install/riscv64-unknown-elf/lib/perch.specs

SRC_DIR := $(ROOT)/testing
BIN_DIR := $(ROOT)/black-parrot-sdk/riscv/bp-tests
VDIR    := $(ROOT)/black-parrot/bp_top/verilator
RESDIR  := $(VDIR)/results/bp_tethered.e_bp_default_cfg

# All tests — add new ones here
TESTS := \
    multithreading_demo \
    mt_csr_isolation_test \
    mt_benchmark \
    mt_frf_isolation_test

# Exported to sub-make (sim.verilator target needs these)
export TOP            := $(ROOT)/black-parrot
export BP_DIR         := $(ROOT)/black-parrot
export BP_INSTALL_DIR := $(ROOT)/install

.PHONY: all sim clean help

# Default: run all tests (simulator must already be built; use 'make sim' to rebuild)
all: $(addprefix run-, $(TESTS))

# Generate explicit compile + run rules for each test.
# Pattern rules combined with .PHONY are unreliable in GNU make, so we use
# eval/foreach to produce one concrete rule per test.
define TEST_RULES
.PHONY: run-$(1)
run-$(1): $(BIN_DIR)/$(1).riscv
	$$(MAKE) -C $$(VDIR) sim.verilator SUITE=bp-tests PROG=$(1)

$(BIN_DIR)/$(1).riscv: $(SRC_DIR)/$(1).c
	$$(CC) -o $$@ $$< $$(CFLAGS)
endef
$(foreach t,$(TESTS),$(eval $(call TEST_RULES,$(t))))

# Rebuild the Verilator simulator from RTL.
# Run this whenever bp_be/bp_fe/bp_top RTL changes.
sim:
	mkdir -p $(RESDIR) $(VDIR)/logs $(VDIR)/reports
	rm -rf $(ROOT)/obj_dir
	cat $(ROOT)/black-parrot/bp_top/flist.vcs \
	    $(ROOT)/black-parrot/bp_top/test/tb/bp_tethered/flist.vcs \
	    > $(RESDIR)/flist.vcs
	sed -i "/^#/d" $(RESDIR)/flist.vcs
	verilator -O2 --x-assign fast --x-initial fast -j 4 --timescale 1ps/1ps \
	  --top-module testbench \
	  $(ROOT)/black-parrot/bp_top/verilator/waiver.vlt \
	  -f $(RESDIR)/flist.vcs \
	  -o $(RESDIR)/simsc \
	  --stats --autoflush \
	  +define+BSG_NO_TIMESCALE --no-assert --Wno-fatal --Wno-lint --Wno-style -Wno-UNOPTFLAT \
	  --binary \
	  -pvalue+perf_enable_p=1  -pvalue+warmup_instr_p=0 \
	  -pvalue+max_instr_p=10000000 -pvalue+max_cycle_p=10000000 \
	  -pvalue+watchdog_enable_p=1  -pvalue+stall_cycles_p=100000 \
	  -pvalue+halt_instr_p=1000    -pvalue+heartbeat_instr_p=10000 \
	  -pvalue+cosim_trace_p=0      -pvalue+cosim_check_p=0 \
	  -pvalue+icache_trace_p=0     -pvalue+dcache_trace_p=0 \
	  -pvalue+vm_trace_p=0         -pvalue+uce_trace_p=0 \
	  -pvalue+lce_trace_p=0        -pvalue+cce_trace_p=0 \
	  -pvalue+dev_trace_p=0        -pvalue+dram_trace_p=0 \
	  -pvalue+sim_clock_period_p=10    -pvalue+sim_reset_cycles_lo_p=0 \
	  -pvalue+sim_reset_cycles_hi_p=20 -pvalue+tb_clock_period_p=2 \
	  -pvalue+tb_reset_cycles_lo_p=0   -pvalue+tb_reset_cycles_hi_p=50 \
	  +define+BP_CFG_FLOWVAR=e_bp_default_cfg \
	  2>&1 | tee $(VDIR)/logs/simsc.bp_tethered.e_bp_default_cfg.log

clean:
	rm -f $(addprefix $(BIN_DIR)/, $(addsuffix .riscv, $(TESTS)))

help:
	@echo "Targets:"
	@echo "  make run-<test>   compile (if stale) and run one test"
	@echo "  make all          compile (if stale) and run all tests"
	@echo "  make sim          rebuild Verilator simulator from RTL"
	@echo "  make clean        remove compiled test binaries"
	@echo ""
	@echo "Available tests:"
	@$(foreach t,$(TESTS),echo "  $(t)";)
