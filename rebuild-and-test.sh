#!/bin/bash

echo "Rebuilding the multithreading_demo test..."

rm black-parrot-sdk/riscv/bp-tests/multithreading_demo.riscv
ls ./black-parrot-sdk/bp-tests/multithreading_demo.riscv
cd ./black-parrot-sdk/bp-tests

# ensures we are using correct riscv64-unknown-elf-gcc
export PATH="/home/coyang/temp/black-parrot-sim/install/bin:$PATH"

riscv64-unknown-elf-gcc -o ../riscv/bp-tests/multithreading_demo.riscv ./src/multithreading_demo.c -I../libperch -march=rv64gc -mabi=lp64d --specs=/../home/coyang/temp/black-parrot-sim/install/riscv64-unknown-elf/lib/dramfs.specs --specs=/../home/coyang/temp/black-parrot-sim/install/riscv64-unknown-elf/lib/perch.specs
ls ../riscv/bp-tests/multithreading_demo.riscv
cd ../../

echo "Rebuilding the simulator"

echo $PWD
make -C black-parrot/bp_top/verilator build.verilator -B

echo "Running the multithreading_demo test..."

make -C black-parrot/bp_top/verilator sim.verilator SUITE=bp-tests PROG=multithreading_demo