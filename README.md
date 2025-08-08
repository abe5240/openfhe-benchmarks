# OpenFHE Benchmarks with Arithmetic Intensity Profiling

A benchmarking suite for OpenFHE (Open Fully Homomorphic Encryption) library that measures arithmetic intensity by combining Intel PIN instrumentation for integer operation counting with hardware performance counters for DRAM traffic measurement.

## Overview

This project provides tools to measure the computational efficiency of FHE operations by calculating their arithmetic intensity (operations per byte of DRAM traffic). It includes:

- **Integer operation counting** using Intel PIN dynamic binary instrumentation
- **DRAM traffic measurement** using hardware performance counters
- **Configurable FHE parameters** via command-line arguments
- **Python integration** for parameter sweeps and analysis
- **Multiple benchmark implementations** (addition, multiplication, matrix operations)

## System Requirements

- Ubuntu 20.04+ (tested on Ubuntu 22.04)
- Intel CPU with uncore PMU support
- OpenFHE library installed (v1.2.0+)
- CMake 3.5.1+
- GCC 11+
- Python 3.8+
- sudo access (for performance counters)

## Installation

### 1. Install OpenFHE

```bash
# Install OpenFHE (if not already installed)
git clone https://github.com/openfheorg/openfhe-development.git
cd openfhe-development
mkdir build && cd build
cmake ..
make -j$(nproc)
sudo make install
```

### 2. Install Profiling Tools

The profiling tools (PIN and DRAM counter) are installed system-wide at `/opt/profiling-tools/`:

```bash
# From the arithmetic-intensity-profiler directory (separate repo)
sudo ./install_profiling_tools.sh

# This installs:
# - Intel PIN to /opt/intel/pin/
# - Custom PIN tool to /opt/profiling-tools/lib/pintool.so
# - DRAM counter header to /opt/profiling-tools/include/dram_counter.hpp
```

### 3. Build the Benchmarks

```bash
cd openfhe-benchmarks
mkdir build
cmake -S . -B build -DBENCH_SOURCE=examples/addition.cpp
cmake --build build
```

## Usage

### Basic Execution

#### Run without PIN (DRAM measurement only):
```bash
sudo ./build/addition --ring-dim=8192 --mult-depth=15
```

#### Run with PIN instrumentation:
```bash
# Using the convenience wrapper
sudo /opt/profiling-tools/bin/run-with-pin ./build/addition --ring-dim=8192

# Or directly
sudo /opt/intel/pin/pin -t /opt/profiling-tools/lib/pintool.so -- \
    ./build/addition --ring-dim=8192
```

### Command-line Parameters

All benchmarks support these parameters:

- `--ring-dim=N` - Ring dimension (default: 65536)
- `--mult-depth=N` - Multiplicative depth (default: 19)
- `--scale-mod-size=N` - Scaling modulus size (default: 50)
- `--check-security=BOOL` - Enable security level check (default: false)
- `--help` - Show usage information

Example:
```bash
sudo ./build/addition --ring-dim=32768 --mult-depth=10 --scale-mod-size=40
```

### Python Integration

#### Single run with profiling:
```python
from bench_runner import BenchmarkRunner

runner = BenchmarkRunner(".", use_pin=True)
result = runner.run("addition", {
    "ring_dim": 16384,
    "mult_depth": 15,
    "scale_mod_size": 45
})

print(f"Arithmetic Intensity: {result.arithmetic_intensity:.6f} ops/byte")
```

#### Parameter sweep:
```bash
python3 bench_runner.py test   # Quick test with PIN
python3 bench_runner.py nopin  # Test without PIN
```

#### Custom parameter sweep:
```python
import subprocess
import re

# Run with different parameters
for ring_dim in [4096, 8192, 16384, 32768]:
    result = subprocess.run(
        ["sudo", "/opt/intel/pin/pin", "-t", "/opt/profiling-tools/lib/pintool.so",
         "--", "./build/addition", f"--ring-dim={ring_dim}"],
        capture_output=True, text=True
    )
    
    # Parse results
    ops = re.search(r"TOTAL: (\d+)", result.stderr)
    dram = re.search(r"DRAM_TOTAL_BYTES=(\d+)", result.stdout)
    
    if ops and dram:
        ai = int(ops.group(1)) / int(dram.group(1))
        print(f"Ring {ring_dim}: {ai:.6f} ops/byte")
```

## Available Benchmarks

- `addition.cpp` - Homomorphic addition of ciphertexts
- `multiplication.cpp` - Homomorphic multiplication with relinearization
- `rotation.cpp` - Ciphertext rotation operations
- `simple-diagonal-method.cpp` - Basic matrix-vector multiplication
- `bsgs-diagonal-method.cpp` - Baby-step giant-step optimization
- `single-hoisted-diagonal-method.cpp` - Single hoisting optimization
- `single-hoisted-bsgs-diagonal-method.cpp` - Combined optimizations

Build any benchmark by specifying its source:
```bash
cmake -S . -B build -DBENCH_SOURCE=examples/multiplication.cpp
cmake --build build
```

## Understanding the Output

### Sample Output
```
=== Configuration ===
Multiplicative depth: 19
Scaling modulus size: 50
Ring dimension: 8192
Security check: DISABLED

=== Addition Benchmark ===
Input 1: (1, 1.1, 1.2, 1.3, 1.4, 1.5, 1.6, 1.7, ... )
Input 2: (2, 2.1, 2.2, 2.3, 2.4, 2.5, 2.6, 2.7, ... )

=== DRAM Traffic (Region) ===
Read : 53.42 MiB
Write: 38.03 MiB
Total: 91.45 MiB

[PIN] Final counts
  ADD: 19230
  SUB: 17274
  MUL: 6
  DIV: 9
  TOTAL: 36519

Arithmetic Intensity: 0.000382 ops/byte
```

### Metrics Explained

- **Integer Operations**: Count of ADD, SUB, MUL, DIV instructions between PIN markers
- **DRAM Traffic**: Total bytes read/written to main memory
- **Arithmetic Intensity**: Ratio of operations to memory traffic (ops/byte)
  - Higher values indicate better computational efficiency
  - Typical FHE operations have very low AI (<1.0) due to large ciphertext sizes

## Project Structure

```
openfhe-benchmarks/
├── CMakeLists.txt              # Build configuration
├── Makefile                    # Convenience targets
├── examples/
│   ├── utils.hpp              # Shared utilities, PIN markers, arg parsing
│   ├── addition.cpp           # Addition benchmark
│   ├── multiplication.cpp     # Multiplication benchmark
│   └── ...                    # Other benchmarks
├── build/                     # Build directory (generated)
├── logs/                      # Profiling outputs (generated)
│   ├── int_counts.out        # PIN operation counts
│   └── dram_counts.out       # DRAM measurements
├── bench_runner.py           # Python benchmark runner
└── test_integration.py       # Integration tests
```

## System Components

### Profiling Tools Location
- `/opt/intel/pin/` - Intel PIN framework
- `/opt/profiling-tools/lib/pintool.so` - Custom PIN instrumentation tool
- `/opt/profiling-tools/include/dram_counter.hpp` - DRAM measurement header

### How It Works

1. **Compilation**: Benchmarks include PIN markers and DRAM counter
2. **Execution**: PIN instruments the binary at runtime
3. **Measurement**: 
   - PIN counts integer operations between markers
   - DRAM counter reads hardware performance counters
4. **Output**: Results written to `logs/` and stdout
5. **Analysis**: Python scripts parse and analyze results

## Troubleshooting

### PIN not counting operations
```bash
# Verify PIN tool is installed
ls -la /opt/profiling-tools/lib/pintool.so

# Test with simple validation
sudo /opt/intel/pin/pin -t /opt/profiling-tools/lib/pintool.so -- \
    /bin/echo "test"

# Check PIN output is being written
cat logs/int_counts.out
```

### DRAM measurements show zero
```bash
# Enable performance counters
sudo sh -c 'echo -1 > /proc/sys/kernel/perf_event_paranoid'

# Verify uncore PMU support
ls /sys/bus/event_source/devices/uncore_imc*
```

### Permission denied errors
```bash
# Most operations require sudo for hardware counter access
sudo ./build/addition --ring-dim=8192
```

### Arithmetic intensity shows 0.000000
The program reads log files after execution. PIN writes its output when the program ends, so you may need to run twice or parse PIN's stderr output directly (see Python examples).

## Performance Considerations

- **PIN overhead**: ~10-100x slowdown when instrumentation is active
- **DRAM counter overhead**: Minimal (<1%)
- **Temporary files**: Created in `/tmp/openfhe_bench_XXXXXX/` and auto-cleaned
- **Log files**: Created in `./logs/` directory

## Advanced Usage

### Custom PIN Markers
Add PIN markers around specific code regions:
```cpp
PIN_MARKER_START();
// Code to profile
auto result = cc->EvalMult(a, b);
PIN_MARKER_END();
```

### Batch Processing
```bash
#!/bin/bash
for dim in 4096 8192 16384 32768; do
    echo "Testing ring dimension: $dim"
    sudo /opt/intel/pin/pin -t /opt/profiling-tools/lib/pintool.so -- \
        ./build/addition --ring-dim=$dim
    
    ops=$(grep -o '[0-9]*' logs/int_counts.out)
    bytes=$(grep DRAM_TOTAL_BYTES logs/dram_counts.out | cut -d= -f2)
    ai=$(echo "scale=6; $ops / $bytes" | bc)
    echo "Ring $dim: AI = $ai ops/byte"
done
```