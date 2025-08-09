# OpenFHE Benchmarks with Arithmetic-Intensity Profiling

Measure FHE efficiency with **three isolated passes** per run:
1) **Latency only**, 2) **DRAM traffic only**, 3) **Integer-op counts only**.  
The Python driver auto-builds binaries, runs warm-ups, executes each pass, parses results, and **falls back to log files** if stdout/stderr parsing changes.

---

## Requirements

- Ubuntu 20.04+ (tested on 22.04)
- Intel CPU with uncore IMC PMU
- OpenFHE ≥ 1.2.0
- CMake ≥ 3.5, GCC ≥ 11, Python ≥ 3.8
- **Passwordless sudo** for perf counters & PIN (**`sudo -n` must work**)

---

## Install

### 1) Install OpenFHE
~~~bash
git clone https://github.com/openfheorg/openfhe-development.git
cd openfhe-development
mkdir build && cd build
cmake ..
make -j"$(nproc)"
sudo make install
~~~

### 2) Install profiling tools (PIN + DRAM counter + pintool)
From this repo:
~~~bash
sudo bash install.sh
# Installs:
#  - PIN → /opt/intel/pin/
#  - pintool.so → /opt/profiling-tools/lib/pintool.so
#  - dram_counter.hpp → /opt/profiling-tools/include/
~~~

> If you modify `src/pintool.cpp` or `src/dram_counter.hpp`, rerun:
> ~~~bash
> sudo bash install.sh
> ~~~

---

## Build the benchmarks

Let Python auto-build, or build manually:
~~~bash
mkdir -p build
cmake -S . -B build -DBENCH_SOURCE=examples/addition.cpp -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
~~~

---

## Quick start

### One benchmark (auto-builds if needed)
~~~bash
python3 benchmark.py addition --ring-dim=8192 --runs=3
~~~

### Thread sweep
~~~bash
python3 benchmark.py multiplication --thread-sweep 1 2 4 8 --ring-dim=8192 --runs=5
~~~

### Parameter sweep
~~~bash
python3 benchmark.py rotation --sweep ring-dim 4096 8192 16384 --threads=4 --runs=5
~~~

### Use a prebuilt executable
~~~bash
python3 benchmark.py --exe ./build/addition --ring-dim=8192 --runs=3
~~~

### Save JSON
~~~bash
python3 benchmark.py addition --ring-dim=8192 --runs=3 --json results.json
~~~

---

## How metrics are collected (exact commands)

Each metric is measured in a **separate process**; no mixing.

- **Latency only**
  ~~~bash
  ./build/<bench> ... --measure=none --quiet=true --skip-verify=true
  ~~~

- **DRAM only** (machine-readable on stdout; Python is whitespace-tolerant)
  ~~~bash
  sudo -n ./build/<bench> ... --measure=dram --quiet=true --skip-verify=true
  # Emits:
  # DRAM_READ_BYTES=<u64>
  # DRAM_WRITE_BYTES=<u64>
  # DRAM_TOTAL_BYTES=<u64>
  ~~~
  If parsing fails, Python falls back to `logs/dram_counts.out`.

- **Integer ops only (PIN)**  
  Counts ADD/SUB/MUL/DIV between `PIN_MARKER_START/END`.
  ~~~bash
  sudo -n /opt/intel/pin/pin -t /opt/profiling-tools/lib/pintool.so -- \
      ./build/<bench> ... --measure=pin --quiet=true --skip-verify=true
  # PIN writes human summary to stderr and logs TOTAL to:
  #   logs/int_counts.out  (line: "TOTAL: <u64>")
  ~~~
  Python parses stderr; if that fails, it falls back to the log.  
  It also accepts the older wording `TOTAL counted:` if present.

---

## Common flags

- `--ring-dim=N`
- `--mult-depth=N`
- `--scale-mod-size=N`
- `--check-security` (boolean)
- `--threads=N`
- `--runs=N` (timing repetitions)
- `--warmup=N` (warm-ups before timing)
- `--sweep <name> v1 v2 ...`
- `--thread-sweep n1 n2 ...`
- `--json path.json`
- `--no-build` (fail if exe is missing)

---

## Output & logs

- Python prints: runtime, DRAM, ops, arithmetic intensity (ops/byte), throughput, bandwidth.
- Always-written logs (used as fallback parsers):
  - `logs/dram_counts.out` → `DRAM_*` lines
  - `logs/int_counts.out`  → `TOTAL: <u64>`

---

## Troubleshooting

- **Only runtime shown (DRAM/PIN say N/A):** passwordless sudo isn’t configured. Ensure `sudo -n true` succeeds for your user on this host.
- **DRAM shows N/A:** enable perf & check IMC devices.
  ~~~bash
  sudo sh -c 'echo -1 > /proc/sys/kernel/perf_event_paranoid'
  ls /sys/bus/event_source/devices/uncore_imc*
  ~~~
- **PIN not counting:** validate install & logs.
  ~~~bash
  ls -l /opt/profiling-tools/lib/pintool.so
  sudo -n /opt/intel/pin/pin -t /opt/profiling-tools/lib/pintool.so -- /bin/true
  cat logs/int_counts.out
  ~~~

---

## Project layout
~~~text
openfhe-benchmarks/
├── benchmark.py                # Orchestrates build + 3-pass measurement
├── examples/
│   ├── utils.hpp               # Args, PIN markers, DRAM counter, helpers
│   ├── addition.cpp            # Example benchmark
│   └── ...
├── src/
│   ├── dram_counter.hpp        # DRAM PMU helper (installed system-wide)
│   └── pintool.cpp             # Intel PIN tool (installed as pintool.so)
├── build/                      # CMake build (generated)
└── logs/
    ├── dram_counts.out         # DRAM fallback
    └── int_counts.out          # PIN fallback
~~~