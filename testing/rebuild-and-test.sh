#!/bin/bash
# Rebuild the Verilator simulator and run all tests.
# For finer control (single test, compile-only), use the Makefile directly:
#   make -C testing run-<test>
#   make -C testing help
set -e
cd "$(dirname "$0")"
make sim
make all
