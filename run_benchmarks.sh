#!/bin/sh
# Reproduce every number in docs/RESULTS.md. ~10 minutes on an 8-core laptop. Close other apps first.
set -e
B=./build/chronos_bench
mkdir -p results
$B --n 1000000 --trials 5 --out results/bench_default.json --csv results/bench_default.csv > results/bench_default.log
$B --n 1000000 --trials 5 --policy 1 --strategies tree,array,adaptive --out results/bench_policy1.json --csv results/bench_policy1.csv > results/bench_policy1.log
$B --n 1000000 --trials 3 --band-hi 4194304 --out results/bench_wide.json --csv results/bench_wide.csv > results/bench_wide.log
# Held-out evaluation: the switching thresholds were frozen before these runs.
# (a) market_like did not exist when the thresholds were tuned; (b) the original six workloads on a fresh seed.
$B --workloads market_like --seed 777 --n 1000000 --trials 5 --out results/bench_heldout.json --csv results/bench_heldout.csv > results/bench_heldout.log
$B --workloads market_like --seed 777 --n 1000000 --trials 3 --band-hi 4194304 --out results/bench_heldout_wide.json --csv results/bench_heldout_wide.csv > results/bench_heldout_wide.log
$B --seed 2024 --n 1000000 --trials 3 --out results/bench_newseed.json --csv results/bench_newseed.csv > results/bench_newseed.log
