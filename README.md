# OpenFHE Benchmarks with Arithmetic-Intensity Profiling

This repo measures FHE efficiency by running **three isolated passes** per benchmark:
1) **Latency only**, 2) **DRAM traffic only**, 3) **Integer-op counts only**.  
`benchmark.py` auto-builds the target, warms up, runs each pass, parses results, and **falls back to log files** if stdout/stderr formats change.

---

## Prereqs (assumed ready)

- OpenFHE installed system-wide.
- Profiling stack installed from the sibling repo **`arithmetic-intensity-profiler`**:
  - You ran: `sudo bash install-profiling-tools.sh`
  - PIN at `/opt/intel/pin/pin`
  - Pintool at `/opt/profiling-tools/lib/pintool.so`
  - DRAM counter header at `/opt/profiling-tools/include/dram_counter.hpp`
- `sudo -n true` works for your user (passwordless sudo) on this host.

Optional quick sanity checks:
~~~bash
ls -l /opt/intel/pin/pin
ls -l /opt/profiling-tools/lib/pintool.so
sudo -n true
~~~

---

## Build (this repo: `openfhe-benchmarks`)

You can let Python build automatically, or build manually:

~~~bash
# Manual build of a specific example
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

Each metric is measured in a **separate process**; nothing is mixed.

- **Latency only**
  ~~~bash
  ./build/<bench> ... --measure=none --quiet=true --skip-verify=true
  ~~~

- **DRAM only** (machine-readable on stdout; whitespace-tolerant parsing)
  ~~~bash
  sudo -n ./build/<bench> ... --measure=dram --quiet=true --skip-verify=true
  # Emits:
  # DRAM_READ_BYTES=<u64>
  # DRAM_WRITE_BYTES=<u64>
  # DRAM_TOTAL_BYTES=<u64>
  ~~~
  If stdout parsing fails, Python falls back to `logs/dram_counts.out`.

- **Integer ops only (PIN)** — counts ADD/SUB/MUL/DIV between `PIN_MARKER_START/END`
  ~~~bash
  sudo -n /opt/intel/pin/pin -t /opt/profiling-tools/lib/pintool.so -- \
      ./build/<bench> ... --measure=pin --quiet=true --skip-verify=true
  # Human summary goes to stderr.
  # Fallback log: logs/int_counts.out (line: "TOTAL: <u64>")
  ~~~
  Python parses stderr; if that fails, it falls back to the log.  
  (Also accepts the older wording `TOTAL counted:` if present.)

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

- Python prints: runtime, DRAM totals, integer ops, arithmetic intensity (ops/byte), throughput, bandwidth.
- Always-written logs (used as fallbacks):
  - `logs/dram_counts.out` → `DRAM_*` lines
  - `logs/int_counts.out`  → `TOTAL: <u64>`

---

## Troubleshooting (minimal)

- **Only runtime shown (DRAM/PIN say N/A):** passwordless sudo isn’t set up. Ensure `sudo -n true` succeeds.
- **DRAM N/A:** enable perf & check IMC devices:
  ~~~bash
  sudo sh -c 'echo -1 > /proc/sys/kernel/perf_event_paranoid'
  ls /sys/bus/event_source/devices/uncore_imc*
  ~~~
- **PIN N/A:** confirm the shared object exists and can run:
  ~~~bash
  ls -l /opt/profiling-tools/lib/pintool.so
  sudo -n /opt/intel/pin/pin -t /opt/profiling-tools/lib/pintool.so -- /bin/true
  cat logs/int_counts.out || true
  ~~~

---

## Project layout
~~~text
openfhe-benchmarks/
├── CMakeLists.txt
├── Makefile
├── README.md
├── benchmark.py                # Orchestrates build + 3-pass measurement
├── build/                      # CMake build (generated)
│   └── addition                # Example output binary (when built)
├── examples/
│   ├── utils.hpp               # Args, PIN markers, DRAM counter, helpers
│   ├── addition.cpp            # Homomorphic addition benchmark
│   ├── multiplication.cpp
│   ├── rotation.cpp
│   ├── simple-diagonal-method.cpp
│   ├── bsgs-diagonal-method.cpp
│   ├── single-hoisted-diagonal-method.cpp
│   └── single-hoisted-bsgs-diagonal-method.cpp
└── logs/
    ├── dram_counts.out         # DRAM fallback
    └── int_counts.out          # PIN fallback

~~~