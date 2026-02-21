#!/bin/bash

ROOT=/home/coyang/temp/black-parrot-sim

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
make -C $ROOT/black-parrot/bp_top/verilator build.verilator -B

# run test
make -C $ROOT/black-parrot/bp_top/verilator sim.verilator SUITE=bp-tests PROG=multithreading_demo