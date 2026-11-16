Each teammate runs the benchmark once on their own machine (laptop plugged in, other apps closed) and commits the JSON here:

    # macOS / Linux
    ./build/chronos_bench --n 1000000 --trials 5 --out results/machines/<yourname>.json
    # Windows (PowerShell, after `cmake --build build --config Release`)
    .\build\Release\chronos_bench.exe --n 1000000 --trials 5 --out results\machines\<yourname>.json

Then `python3 scripts/make_report.py` adds a cross-machine table to docs/RESULTS.md. Do not edit the JSON by hand.
