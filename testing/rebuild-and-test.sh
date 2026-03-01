#!/bin/bash

ROOT=/home/coyang/black-parrot-sim
export PATH=$ROOT/install/bin:/usr/bin:/bin:$PATH
# TOP and BP_DIR must point to the black-parrot submodule so its Makefile.env
# is found and $BP_DIR expands correctly in flist.vcs.
# BP_INSTALL_DIR overrides to the meta-repo's install/ so the right verilator
# and toolchain are found.
export TOP=$ROOT/black-parrot
export BP_DIR=$ROOT/black-parrot
export BP_INSTALL_DIR=$ROOT/install

# remove old test binary
rm $ROOT/black-parrot-sdk/riscv/bp-tests/multithreading_demo.riscv

# make sure it is gone (this isn't necessary)
ls $ROOT/black-parrot-sdk/riscv/bp-tests/multithreading_demo.riscv

# ensures we are using correct riscv64-unknown-elf-gcc
$ROOT/install/bin/riscv64-unknown-elf-gcc \
  -o $ROOT/black-parrot-sdk/riscv/bp-tests/multithreading_demo.riscv \
  $ROOT/testing/multithreading_demo.c \
  -I$ROOT/black-parrot-sdk/libperch \
  -march=rv64gc \
  -mabi=lp64d \
  --specs=$ROOT/install/riscv64-unknown-elf/lib/dramfs.specs \
  --specs=$ROOT/install/riscv64-unknown-elf/lib/perch.specs

# make sure it is there
ls $ROOT/black-parrot-sdk/riscv/bp-tests/multithreading_demo.riscv

# rebuild simulator
# make -C $ROOT/black-parrot/bp_top/verilator build.verilator -B

# Note: 'make build.verilator' fails because make doesn't export BP_DIR to the
# verilator subprocess (needed to expand $BP_DIR in flist.vcs). Run verilator
# directly with BP_DIR in env and an absolute path to flist.vcs instead.
RESDIR=$ROOT/black-parrot/bp_top/verilator/results/bp_tethered.e_bp_default_cfg
mkdir -p $RESDIR
# Remove stale verilator intermediate files to force a clean C++ rebuild
rm -rf $ROOT/obj_dir
# Regenerate flist.vcs
cat $ROOT/black-parrot/bp_top/flist.vcs $ROOT/black-parrot/bp_top/test/tb/bp_tethered/flist.vcs > $RESDIR/flist.vcs
sed -i "/^#/d" $RESDIR/flist.vcs
mkdir -p $ROOT/black-parrot/bp_top/verilator/logs $ROOT/black-parrot/bp_top/verilator/reports
verilator -O2 --x-assign fast --x-initial fast -j 4 --timescale 1ps/1ps \
  --top-module testbench \
  $ROOT/black-parrot/bp_top/verilator/waiver.vlt \
  -f $RESDIR/flist.vcs \
  -o $RESDIR/simsc \
  --stats --autoflush \
  +define+BSG_NO_TIMESCALE --no-assert --Wno-fatal --Wno-lint --Wno-style -Wno-UNOPTFLAT \
  --binary \
  -pvalue+perf_enable_p=1 -pvalue+warmup_instr_p=0 \
  -pvalue+max_instr_p=10000000 -pvalue+max_cycle_p=10000000 \
  -pvalue+watchdog_enable_p=1 -pvalue+stall_cycles_p=100000 \
  -pvalue+halt_instr_p=1000 -pvalue+heartbeat_instr_p=10000 \
  -pvalue+cosim_trace_p=0 -pvalue+cosim_check_p=0 \
  -pvalue+icache_trace_p=0 -pvalue+dcache_trace_p=0 \
  -pvalue+vm_trace_p=0 -pvalue+uce_trace_p=0 \
  -pvalue+lce_trace_p=0 -pvalue+cce_trace_p=0 \
  -pvalue+dev_trace_p=0 -pvalue+dram_trace_p=0 \
  -pvalue+sim_clock_period_p=10 -pvalue+sim_reset_cycles_lo_p=0 \
  -pvalue+sim_reset_cycles_hi_p=20 -pvalue+tb_clock_period_p=2 \
  -pvalue+tb_reset_cycles_lo_p=0 -pvalue+tb_reset_cycles_hi_p=50 \
  +define+BP_CFG_FLOWVAR=e_bp_default_cfg \
  2>&1 | tee $ROOT/black-parrot/bp_top/verilator/logs/simsc.bp_tethered.e_bp_default_cfg.log

# compile Phase 2A tests
$ROOT/install/bin/riscv64-unknown-elf-gcc \
  -o $ROOT/black-parrot-sdk/riscv/bp-tests/mt_csr_isolation_test.riscv \
  $ROOT/testing/mt_csr_isolation_test.c \
  -I$ROOT/black-parrot-sdk/libperch \
  -march=rv64gc \
  -mabi=lp64d \
  --specs=$ROOT/install/riscv64-unknown-elf/lib/dramfs.specs \
  --specs=$ROOT/install/riscv64-unknown-elf/lib/perch.specs

$ROOT/install/bin/riscv64-unknown-elf-gcc \
  -o $ROOT/black-parrot-sdk/riscv/bp-tests/mt_benchmark.riscv \
  $ROOT/testing/mt_benchmark.c \
  -I$ROOT/black-parrot-sdk/libperch \
  -march=rv64gc \
  -mabi=lp64d \
  --specs=$ROOT/install/riscv64-unknown-elf/lib/dramfs.specs \
  --specs=$ROOT/install/riscv64-unknown-elf/lib/perch.specs

# run tests
make -C $ROOT/black-parrot/bp_top/verilator sim.verilator SUITE=bp-tests PROG=multithreading_demo
make -C $ROOT/black-parrot/bp_top/verilator sim.verilator SUITE=bp-tests PROG=mt_csr_isolation_test
make -C $ROOT/black-parrot/bp_top/verilator sim.verilator SUITE=bp-tests PROG=mt_benchmark

# compile and run mt_regfile_test
# $ROOT/install/bin/riscv64-unknown-elf-gcc \
#   -o $ROOT/black-parrot-sdk/riscv/bp-tests/mt_regfile_test.riscv \
#   $ROOT/testing/mt_regfile_test.c \
#   -I$ROOT/black-parrot-sdk/libperch \
#   -march=rv64gc \
#   -mabi=lp64d \
#   --specs=$ROOT/install/riscv64-unknown-elf/lib/dramfs.specs \
#   --specs=$ROOT/install/riscv64-unknown-elf/lib/perch.specs

# make -C $ROOT/black-parrot/bp_top/verilator sim.verilator SUITE=bp-tests PROG=mt_regfile_test