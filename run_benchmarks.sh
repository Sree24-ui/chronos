#!/bin/sh
# Reproduce every number in docs/RESULTS.md. ~10 minutes on an 8-core laptop. Close other apps first.
set -e
B=./build/chronos_bench
mkdir -p results
$B --n 1000000 --trials 5 --out results/bench_default.json --csv results/bench_default.csv > results/bench_default.log
$B --n 1000000 --trials 5 --policy 1 --strategies tree,array,adaptive --out results/bench_policy1.json --csv results/bench_policy1.csv > results/bench_policy1.log
$B --n 1000000 --trials 3 --band-hi 4194304 --out results/bench_wide.json --csv results/bench_wide.csv > results/bench_wide.log
