#!/usr/bin/env python3
"""
benchmark.py - OpenFHE Benchmark Runner with Arithmetic Intensity Measurement
Enhanced with automatic building, warm-up runs, and smart rebuilds.

Usage Examples:
    # Command line
    python3 benchmark.py addition --ring-dim=8192 --runs=5

    # Python API
    from benchmark import Benchmark

    bench = Benchmark("addition")
    result = bench.run(runs=10, ring_dim=8192)  # 10 runs for better stats
    result.print_summary()

    # Quick test with single run
    result = bench.run(runs=1, ring_dim=4096)

    # Parameter sweep with custom runs
    results = bench.parameter_sweep("ring_dim", [4096, 8192], runs=5)
"""

import subprocess
import time
import re
import statistics
import json
import sys
import os
from pathlib import Path
from typing import Dict, Any, Optional, List
from dataclasses import dataclass, asdict


@dataclass
class BenchmarkResult:
    """Benchmark measurement results"""
    # Performance metrics
    runtime_sec: float = 0.0
    runtime_stdev: float = 0.0

    # Memory metrics
    dram_read_bytes: Optional[int] = None
    dram_write_bytes: Optional[int] = None
    dram_total_bytes: Optional[int] = None

    # Computation metrics
    integer_ops: Optional[int] = None
    ops_breakdown: Dict[str, int] = None

    # Efficiency metrics
    arithmetic_intensity: Optional[float] = None
    throughput_gops: Optional[float] = None
    bandwidth_gb_sec: Optional[float] = None

    # Parameters used
    parameters: Dict[str, Any] = None

    def __post_init__(self):
        """Calculate derived metrics"""
        if self.ops_breakdown is None:
            self.ops_breakdown = {}

        # Arithmetic intensity (ops per byte)
        if self.integer_ops and self.dram_total_bytes:
            self.arithmetic_intensity = (
                self.integer_ops / self.dram_total_bytes
            )

        # Throughput (billion ops per second)
        if self.integer_ops and self.runtime_sec > 0:
            self.throughput_gops = (self.integer_ops / 1e9) / self.runtime_sec

        # Memory bandwidth
        if self.dram_total_bytes and self.runtime_sec > 0:
            self.bandwidth_gb_sec = (
                self.dram_total_bytes / (1 << 30)
            ) / self.runtime_sec

    def to_json(self) -> str:
        """Export results as JSON"""
        return json.dumps(asdict(self), indent=2)

    def print_summary(self):
        """Print human-readable summary"""
        print("\n" + "=" * 60)
        print("BENCHMARK RESULTS")
        print("=" * 60)

        if self.parameters:
            print("\nParameters:")
            for key, value in self.parameters.items():
                print(f"  {key}: {value}")

        print(f"\nPerformance:")
        print(
            f"  Runtime: {self.runtime_sec:.3f} ± "
            f"{self.runtime_stdev:.3f} seconds"
        )

        if self.dram_total_bytes:
            print(f"\nMemory Traffic:")
            print(f"  Read:  {self._format_bytes(self.dram_read_bytes)}")
            print(f"  Write: {self._format_bytes(self.dram_write_bytes)}")
            print(f"  Total: {self._format_bytes(self.dram_total_bytes)}")

        if self.integer_ops:
            print(f"\nComputation:")
            print(
                f"  Total operations: {self._format_ops(self.integer_ops)}"
            )
            if self.ops_breakdown:
                for op, count in sorted(self.ops_breakdown.items()):
                    percentage = (count / self.integer_ops) * 100
                    print(
                        f"    {op}: {self._format_ops(count)} "
                        f"({percentage:.1f}%)"
                    )

        print(f"\nEfficiency Metrics:")
        if self.arithmetic_intensity is not None:
            if self.arithmetic_intensity >= 1.0:
                print(
                    "  Arithmetic Intensity: "
                    f"{self.arithmetic_intensity:.2f} ops/byte"
                )
            elif self.arithmetic_intensity >= 0.01:
                print(
                    "  Arithmetic Intensity: "
                    f"{self.arithmetic_intensity:.4f} ops/byte"
                )
            else:
                print(
                    "  Arithmetic Intensity: "
                    f"{self.arithmetic_intensity:.6f} ops/byte"
                )

        if self.throughput_gops:
            print(f"  Throughput: {self._format_throughput(self.throughput_gops)}")  # noqa: E501

        if self.bandwidth_gb_sec:
            print(
                f"  Memory Bandwidth: "
                f"{self._format_bandwidth(self.bandwidth_gb_sec)}"
            )

        print("=" * 60)

    def _format_bytes(self, bytes_val):
        """Format bytes in human-readable form"""
        if bytes_val is None:
            return "N/A"
        if bytes_val >= (1 << 30):
            return f"{bytes_val / (1 << 30):.2f} GiB"
        elif bytes_val >= (1 << 20):
            return f"{bytes_val / (1 << 20):.2f} MiB"
        elif bytes_val >= (1 << 10):
            return f"{bytes_val / (1 << 10):.2f} KiB"
        else:
            return f"{bytes_val} bytes"

    def _format_ops(self, ops_count):
        """Format operation count in human-readable form"""
        if ops_count is None:
            return "N/A"
        if ops_count >= 1e12:
            return f"{ops_count/1e12:.2f}T"
        elif ops_count >= 1e9:
            return f"{ops_count/1e9:.2f}B"
        elif ops_count >= 1e6:
            return f"{ops_count/1e6:.2f}M"
        elif ops_count >= 1e3:
            return f"{ops_count/1e3:.1f}K"
        else:
            return f"{ops_count:,}"

    def _format_throughput(self, gops):
        """Format throughput in appropriate units"""
        if gops >= 1000:
            return f"{gops/1000:.2f} Tops/s"
        elif gops >= 1:
            return f"{gops:.2f} Gops/s"
        elif gops >= 0.001:
            return f"{gops*1000:.2f} Mops/s"
        elif gops >= 0.000001:
            return f"{gops*1e6:.2f} Kops/s"
        else:
            return f"{gops*1e9:.1f} ops/s"

    def _format_bandwidth(self, gb_sec):
        """Format bandwidth in appropriate units"""
        if gb_sec >= 1000:
            return f"{gb_sec/1000:.2f} TB/s"
        elif gb_sec >= 1:
            return f"{gb_sec:.2f} GB/s"
        elif gb_sec >= 0.001:
            return f"{gb_sec*1000:.2f} MB/s"
        else:
            return f"{gb_sec*1e6:.1f} KB/s"


class Benchmark:
    """OpenFHE benchmark runner with automatic building"""

    def __init__(
        self, benchmark_name: str = None, executable_path: str = None,
        pin_path: str = "/opt/intel/pin/pin",
        pintool_path: str = "/opt/profiling-tools/lib/pintool.so",
        auto_build: bool = True, warmup_runs: int = 1
    ):
        """
        Initialize benchmark runner

        Args:
            benchmark_name: Name (e.g., 'addition', 'multiplication')
            executable_path: Direct path to executable
            pin_path: Path to PIN binary
            pintool_path: Path to PIN tool
            auto_build: Auto build if not found
            warmup_runs: Warm-up runs before timing
        """

        # Determine project root
        self.project_root = Path.cwd()
        self.build_dir = self.project_root / "build"
        self.examples_dir = self.project_root / "examples"
        self.warmup_runs = warmup_runs

        if benchmark_name:
            # Build from benchmark name
            self.benchmark_name = benchmark_name
            self.source_file = self.examples_dir / f"{benchmark_name}.cpp"
            self.executable = self.build_dir / benchmark_name

            # Check if build is needed
            if auto_build and self._needs_rebuild():
                self._build_benchmark()
            elif not self.executable.exists():
                raise FileNotFoundError(
                    f"Executable not found: {self.executable}\n"
                    f"Run with auto_build=True or build manually"
                )
        elif executable_path:
            # Use provided executable
            self.executable = Path(executable_path).resolve()
            self.benchmark_name = self.executable.stem
            if not self.executable.exists():
                raise FileNotFoundError(
                    f"Executable not found: {executable_path}"
                )
        else:
            raise ValueError(
                "Must provide either benchmark_name or executable_path"
            )

        self.pin_path = pin_path
        self.pintool_path = pintool_path

        # Verify tools are available
        self._verify_tools()

    def _needs_rebuild(self) -> bool:
        """Check if source is newer than executable"""
        if not self.executable.exists():
            return True
        if not self.source_file.exists():
            return False

        # Check modification times
        src_mtime = self.source_file.stat().st_mtime
        exe_mtime = self.executable.stat().st_mtime
        return src_mtime > exe_mtime

    def _check_sudo(self) -> bool:
        """Check if we can run sudo without password"""
        try:
            result = subprocess.run(
                ["sudo", "-n", "true"], capture_output=True, timeout=1
            )
            return result.returncode == 0
        except Exception:
            return False

    def _build_benchmark(self):
        """Build the benchmark from source"""
        rel = self.source_file.relative_to(self.project_root)
        print(f"🔨 Building {self.benchmark_name} from {rel}...")

        if not self.source_file.exists():
            raise FileNotFoundError(f"Source file not found: {self.source_file}")

        if not self._needs_rebuild():
            rel_exe = self.executable.relative_to(self.project_root)
            print(f"✅ Up to date: {rel_exe}")
            return

        self.build_dir.mkdir(exist_ok=True)

        cmake_cmd = [
            "cmake", "-S", str(self.project_root), "-B", str(self.build_dir),
            "-DCMAKE_BUILD_TYPE=Release",
            f"-DBENCH_SOURCE=examples/{self.benchmark_name}.cpp",
        ]

        print(f"  Running: {' '.join(cmake_cmd)}")
        result = subprocess.run(cmake_cmd, capture_output=True, text=True)
        if result.returncode != 0:
            print(f"CMake configuration failed:\n{result.stderr}")
            raise RuntimeError("Failed to configure build")

        num_jobs = os.cpu_count() or 1
        build_cmd = [
            "cmake", "--build", str(self.build_dir), "--", f"-j{num_jobs}"
        ]
        print(f"  Running: {' '.join(build_cmd)}")
        result = subprocess.run(build_cmd, capture_output=True, text=True)
        if result.returncode != 0:
            print(f"Build failed:\n{result.stderr}")
            raise RuntimeError("Failed to build benchmark")

        rel_exe = self.executable.relative_to(self.project_root)
        print(f"✅ Successfully built {rel_exe}")

    def _verify_tools(self):
        """Check if required tools are installed"""
        if not Path(self.pin_path).exists():
            print(f"⚠️  Warning: PIN not found at {self.pin_path}")
            print("   Operation counting will not be available")
            self.pin_available = False
        else:
            self.pin_available = True

        if not Path(self.pintool_path).exists():
            print(f"⚠️  Warning: PIN tool not found at {self.pintool_path}")
            self.pin_available = False

    def run(self, runs: int = 3, **params) -> BenchmarkResult:
        """
        Run benchmark with specified parameters
        """
        # Extract thread count if specified
        threads = params.pop('threads', None)

        # Convert parameters to command-line arguments
        args = []
        for key, value in params.items():
            param_name = key.replace('_', '-')
            if isinstance(value, bool):
                value = 'true' if value else 'false'
            args.append(f"--{param_name}={value}")

        if threads:
            args.append(f"--threads={threads}")

        result = BenchmarkResult(
            parameters={**params, 'threads': threads} if threads else params
        )

        # Measure performance
        print(f"\n🔬 Benchmarking {self.benchmark_name}")
        if params or threads:
            display_params = {**params}
            if threads:
                display_params['threads'] = threads
            print(f"   Parameters: {display_params}")

        perf_data = self._measure_performance(args, threads, runs)
        if perf_data:
            result.runtime_sec = perf_data['mean']
            result.runtime_stdev = perf_data['stdev']

        # Measure DRAM traffic
        dram_data = self._measure_dram(args, threads)
        if dram_data:
            result.dram_read_bytes = dram_data.get('read')
            result.dram_write_bytes = dram_data.get('write')
            result.dram_total_bytes = dram_data.get('total')

        # Count operations (if PIN available), else print reason (item 5)
        if self.pin_available and self._check_sudo():
            ops_data = self._count_operations(args, threads)
            if ops_data:
                result.integer_ops = ops_data['total']
                result.ops_breakdown = ops_data['breakdown']
        else:
            print("  🔢 Counting operations...", end=" ")
            if not self.pin_available:
                print("N/A (PIN tool unavailable)")
            elif not self._check_sudo():
                print("N/A (no sudo)")

        # Recalculate derived metrics
        result.__post_init__()

        return result

    def _prepare_env(self, threads: Optional[int]) -> Dict[str, str]:
        """Prepare environment variables with thread control"""
        env = os.environ.copy()
        if threads:
            env['OMP_NUM_THREADS'] = str(threads)
            env['OPENBLAS_NUM_THREADS'] = '1'
            env['MKL_NUM_THREADS'] = '1'
            env['NUMEXPR_NUM_THREADS'] = '1'
        return env

    def _warmup(self, cmd: List[str], env: Dict[str, str]):
        """Run warm-up iterations to stabilize performance"""
        for _ in range(self.warmup_runs):
            try:
                subprocess.run(
                    cmd, capture_output=True, check=False, env=env, timeout=30
                )
            except Exception:
                pass  # Ignore warm-up failures

    def _measure_performance(
        self, args: List[str], threads: Optional[int] = None, runs: int = 3
    ) -> Dict[str, float]:
        """Measure runtime performance"""
        runs_text = f"{runs} run" if runs == 1 else f"{runs} runs"
        thread_text = f" (threads={threads})" if threads else ""
        print(
            f"  ⏱️  Measuring performance ({runs_text}){thread_text}...",
            end=" "
        )

        times = []
        cmd = [
            str(self.executable),
        ] + args + ["--measure=none", "--quiet=true", "--skip-verify=true"]
        env = self._prepare_env(threads)

        if self.warmup_runs > 0:
            self._warmup(cmd, env)

        for _ in range(runs):
            start = time.perf_counter()
            try:
                result = subprocess.run(
                    cmd, capture_output=True, check=True, env=env, timeout=300
                )
                times.append(time.perf_counter() - start)
            except subprocess.CalledProcessError as e:
                print(f"\n  ⚠️  Performance run failed")
                if e.stderr:
                    msg = e.stderr.decode('utf-8', errors='ignore')[:200]
                    print(f"  Error: {msg}")
                return None
            except subprocess.TimeoutExpired:
                print("\n  ⚠️  Performance run timed out")
                return None

        if times:
            out = {
                'mean': statistics.mean(times),
                'stdev': statistics.stdev(times) if len(times) > 1 else 0.0,
            }
            if runs == 1:
                print(f"{out['mean']:.3f}s")
            else:
                print(f"{out['mean']:.3f}±{out['stdev']:.3f}s")
            return out
        return None

    # ---------- Fallback parsers (item 3) ----------

    def _fallback_parse_dram_log(self) -> Optional[Dict[str, int]]:
        """Read logs/dram_counts.out and parse DRAM_* lines."""
        path = self.project_root / "logs" / "dram_counts.out"
        if not path.exists():
            return None

        dram_data: Dict[str, int] = {}
        try:
            with path.open("r", encoding="utf-8", errors="ignore") as f:
                for raw in f:
                    line = raw.strip()
                    m = re.match(r'^DRAM_READ_BYTES\s*=\s*(\d+)\s*$', line)
                    if m:
                        dram_data['read'] = int(m.group(1))
                        continue
                    m = re.match(r'^DRAM_WRITE_BYTES\s*=\s*(\d+)\s*$', line)
                    if m:
                        dram_data['write'] = int(m.group(1))
                        continue
                    m = re.match(r'^DRAM_TOTAL_BYTES\s*=\s*(\d+)\s*$', line)
                    if m:
                        dram_data['total'] = int(m.group(1))
            if dram_data.get('total'):
                return dram_data
        except Exception:
            return None
        return None

    def _fallback_parse_pin_log(self) -> Optional[Dict[str, Any]]:
        """Read logs/int_counts.out and parse TOTAL (and optionally parts)."""
        path = self.project_root / "logs" / "int_counts.out"
        if not path.exists():
            return None

        out: Dict[str, Any] = {'total': 0, 'breakdown': {}}
        try:
            with path.open("r", encoding="utf-8", errors="ignore") as f:
                text = f.read()
            # Accept both historical and current wording (item 4)
            m = re.search(r'\bTOTAL(?: counted)?:\s*(\d+)', text)
            if m:
                out['total'] = int(m.group(1))
            if out['total'] > 0:
                return out
        except Exception:
            return None
        return None

    # ---------- DRAM measurement ----------

    def _measure_dram(
        self, args: List[str], threads: Optional[int] = None
    ) -> Optional[Dict[str, int]]:
        """Measure DRAM traffic using hardware counters"""
        print("  💾 Measuring memory traffic...", end=" ")

        if not self._check_sudo():
            print("N/A (no sudo)")
            return None

        cmd = [
            "sudo", "-n", str(self.executable)
        ] + args + ["--measure=dram", "--quiet=true", "--skip-verify=true"]
        env = self._prepare_env(threads)

        dram_data: Optional[Dict[str, int]] = None

        try:
            result = subprocess.run(
                cmd, capture_output=True, check=True, text=True,
                env=env, timeout=300
            )

            data: Dict[str, int] = {}
            for line in result.stdout.splitlines():
                line = line.strip()
                m = re.match(r'^DRAM_READ_BYTES\s*=\s*(\d+)\s*$', line)
                if m:
                    data['read'] = int(m.group(1))
                    continue
                m = re.match(r'^DRAM_WRITE_BYTES\s*=\s*(\d+)\s*$', line)
                if m:
                    data['write'] = int(m.group(1))
                    continue
                m = re.match(r'^DRAM_TOTAL_BYTES\s*=\s*(\d+)\s*$', line)
                if m:
                    data['total'] = int(m.group(1))

            if data.get('total'):
                dram_data = data

        except subprocess.CalledProcessError as e:
            # Try fallback even if the process errored
            dram_data = self._fallback_parse_dram_log()
            if not dram_data:
                print("failed")
                if e.stderr:
                    print(f"  Error: {e.stderr[:200]}")
                return None
        except subprocess.TimeoutExpired:
            # Try fallback on timeout too
            dram_data = self._fallback_parse_dram_log()
            if not dram_data:
                print("timeout")
                return None

        if not dram_data:
            # Fallback to log file (item 3)
            dram_data = self._fallback_parse_dram_log()

        if dram_data and dram_data.get('total'):
            print(f"{dram_data['total'] / (1<<20):.1f} MiB")
            return dram_data

        print("N/A (parse failed)")
        return None

    # ---------- PIN op counting ----------

    def _count_operations(
        self, args: List[str], threads: Optional[int] = None
    ) -> Optional[Dict[str, Any]]:
        """Count operations using PIN instrumentation"""
        print("  🔢 Counting operations...", end=" ")

        cmd = [
            "sudo", "-n", self.pin_path, "-t", self.pintool_path, "--",
            str(self.executable),
        ] + args + ["--measure=pin", "--quiet=true", "--skip-verify=true"]
        env = self._prepare_env(threads)

        ops_data: Dict[str, Any] = {'total': 0, 'breakdown': {}}

        try:
            result = subprocess.run(
                cmd, capture_output=True, check=True, text=True,
                env=env, timeout=600
            )

            stderr_text = result.stderr

            # Accept both "TOTAL:" and "TOTAL counted:" (item 4)
            total_match = re.search(
                r'\bTOTAL(?: counted)?:\s*(\d+)', stderr_text
            )
            if total_match:
                ops_data['total'] = int(total_match.group(1))

            for op in ['ADD', 'SUB', 'MUL', 'DIV']:
                m = re.search(rf'\b{op}:\s*(\d+)', stderr_text)
                if m:
                    ops_data['breakdown'][op] = int(m.group(1))

        except subprocess.CalledProcessError as e:
            # Fallback to the log file (item 3)
            fb = self._fallback_parse_pin_log()
            if fb and fb.get('total', 0) > 0:
                print(f"{fb['total']:,} ops (from log)")
                return fb
            print("failed")
            if e.stderr:
                print(f"  Error: {e.stderr[:200]}")
            return None
        except subprocess.TimeoutExpired:
            fb = self._fallback_parse_pin_log()
            if fb and fb.get('total', 0) > 0:
                print(f"{fb['total']:,} ops (from log)")
                return fb
            print("timeout")
            return None

        if ops_data['total'] <= 0:
            # Fallback if parsing stderr yielded nothing
            fb = self._fallback_parse_pin_log()
            if fb and fb.get('total', 0) > 0:
                print(f"{fb['total']:,} ops (from log)")
                return fb
            print("N/A (parse failed)")
            return None

        print(f"{ops_data['total']:,} ops")
        return ops_data

    # ---------- Sweeps and comparison ----------

    def thread_sweep(
        self, thread_counts: List[int], runs: int = 3,
        fixed_params: Dict[str, Any] = None
    ) -> List[BenchmarkResult]:
        """Run benchmark across different thread counts"""
        results = []
        fixed_params = fixed_params or {}

        print(f"\n🧵 Thread scaling sweep")
        print(f"   Thread counts: {thread_counts}")
        print(f"   Runs per test: {runs}")
        if fixed_params:
            print(f"   Fixed parameters: {fixed_params}")

        for threads in thread_counts:
            params = {**fixed_params, 'threads': threads}
            result = self.run(runs=runs, **params)
            results.append(result)

        return results

    def parameter_sweep(
        self, param_name: str, values: List[Any], runs: int = 3,
        fixed_params: Dict[str, Any] = None
    ) -> List[BenchmarkResult]:
        """Run benchmark across multiple parameter values"""
        results = []
        fixed_params = fixed_params or {}

        print(f"\n📊 Parameter sweep: {param_name}")
        print(f"   Values: {values}")
        print(f"   Runs per test: {runs}")
        if fixed_params:
            print(f"   Fixed: {fixed_params}")

        for value in values:
            params = {**fixed_params, param_name: value}
            result = self.run(runs=runs, **params)
            results.append(result)

        return results

    def compare_results(
        self, results: List[BenchmarkResult], param_name: str = None
    ):
        """Print comparison table with auto-sized columns"""
        headers = [
            "Parameters", "Runtime (s)", "DRAM", "Operations", "AI",
            "Throughput", "Bandwidth",
        ]
        rows = []

        for result in results:
            if param_name and result.parameters:
                pval = result.parameters.get(param_name, 'N/A')
                param_str = f"{param_name}={pval}"
                if 'threads' in result.parameters:
                    param_str += f", t={result.parameters['threads']}"
            else:
                param_str = str(result.parameters)[:30]

            runtime = (
                f"{result.runtime_sec:.3f}" if result.runtime_sec else "N/A"
            )

            if result.dram_total_bytes:
                dtb = result.dram_total_bytes
                if dtb >= (1 << 30):
                    dram = f"{dtb/(1<<30):.2f} GiB"
                elif dtb >= (1 << 20):
                    dram = f"{dtb/(1<<20):.1f} MiB"
                elif dtb >= (1 << 10):
                    dram = f"{dtb/(1<<10):.1f} KiB"
                else:
                    dram = f"{dtb} B"
            else:
                dram = "N/A"

            if result.integer_ops:
                iops = result.integer_ops
                if iops >= 1e12:
                    ops = f"{iops/1e12:.2f}T"
                elif iops >= 1e9:
                    ops = f"{iops/1e9:.2f}B"
                elif iops >= 1e6:
                    ops = f"{iops/1e6:.2f}M"
                elif iops >= 1e3:
                    ops = f"{iops/1e3:.1f}K"
                else:
                    ops = str(iops)
            else:
                ops = "N/A"

            if result.arithmetic_intensity:
                ai_val = result.arithmetic_intensity
                if ai_val >= 1.0:
                    ai = f"{ai_val:.2f}"
                elif ai_val >= 0.01:
                    ai = f"{ai_val:.4f}"
                else:
                    ai = f"{ai_val:.6f}"
            else:
                ai = "N/A"

            if result.throughput_gops:
                tg = result.throughput_gops
                if tg >= 1000:
                    throughput = f"{tg/1000:.2f} Tops/s"
                elif tg >= 1:
                    throughput = f"{tg:.2f} Gops/s"
                elif tg >= 0.001:
                    throughput = f"{tg*1000:.2f} Mops/s"
                elif tg >= 0.000001:
                    throughput = f"{tg*1e6:.2f} Kops/s"
                else:
                    throughput = f"{tg*1e9:.1f} ops/s"
            else:
                throughput = "N/A"

            if result.bandwidth_gb_sec:
                bw = result.bandwidth_gb_sec
                if bw >= 1000:
                    bandwidth = f"{bw/1000:.2f} TB/s"
                elif bw >= 1:
                    bandwidth = f"{bw:.2f} GB/s"
                elif bw >= 0.001:
                    bandwidth = f"{bw*1000:.2f} MB/s"
                else:
                    bandwidth = f"{bw*1e6:.1f} KB/s"
            else:
                bandwidth = "N/A"

            rows.append(
                [param_str, runtime, dram, ops, ai, throughput, bandwidth]
            )

        widths = []
        for i, header in enumerate(headers):
            max_width = len(header)
            for row in rows:
                max_width = max(max_width, len(row[i]))
            widths.append(max_width + 2)

        total_width = sum(widths) + len(widths) - 1

        print("\n" + "=" * total_width)
        print("COMPARISON TABLE")
        print("=" * total_width)

        header_line = ""
        for i, header in enumerate(headers):
            header_line += header.ljust(widths[i])
        print(header_line)
        print("-" * total_width)

        for row in rows:
            row_line = ""
            for i, cell in enumerate(row):
                row_line += cell.ljust(widths[i])
            print(row_line)

        print("=" * total_width)


def main():
    """Command-line interface"""
    import argparse

    parser = argparse.ArgumentParser(
        description="OpenFHE Benchmark Runner with Auto-Build",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # Basic benchmark (auto-builds if needed)
  python3 benchmark.py addition --ring-dim=8192

  # Run with more repetitions for better statistics
  python3 benchmark.py addition --ring-dim=8192 --runs=10

  # Thread scaling sweep with 5 runs each
  python3 benchmark.py multiplication --thread-sweep 1 2 4 8 \
      --ring-dim=8192 --runs=5

  # Parameter sweep with custom runs
  python3 benchmark.py rotation --sweep ring-dim 4096 8192 16384 \
      --threads=4 --runs=7

  # Quick single run for testing
  python3 benchmark.py addition --ring-dim=8192 --runs=1

  # Use pre-built executable
  python3 benchmark.py --exe ./build/addition --ring-dim=8192

  # Save results to JSON
  python3 benchmark.py addition --ring-dim=8192 --json results.json
        """,
    )

    parser.add_argument(
        "benchmark", nargs='?',
        help="Benchmark name (e.g., 'addition', 'multiplication')"
    )
    parser.add_argument(
        "--exe",
        help="Path to pre-built executable (alternative to benchmark name)"
    )

    # Benchmark parameters
    parser.add_argument("--ring-dim", type=int, help="Ring dimension")
    parser.add_argument("--mult-depth", type=int, help="Multiplicative depth")
    parser.add_argument("--scale-mod-size", type=int,
                        help="Scaling modulus size")
    parser.add_argument(
        "--check-security", action='store_true',
        help="Enable security check"
    )
    parser.add_argument("--threads", type=int,
                        help="Number of OpenMP threads")

    # Performance measurement
    parser.add_argument(
        "--runs", type=int, default=3,
        help="Number of runs for timing statistics (default: 3)"
    )
    parser.add_argument(
        "--warmup", type=int, default=1,
        help="Warm-up runs before timing (default: 1)"
    )

    # Sweep options
    parser.add_argument(
        "--thread-sweep", nargs='+', type=int,
        help="Thread counts for scaling test"
    )
    parser.add_argument(
        "--sweep", nargs='+',
        help="Parameter sweep: name value1 value2 ..."
    )

    # Output options
    parser.add_argument("--json", help="Save results to JSON file")
    parser.add_argument(
        "--no-build", action='store_true',
        help="Don't auto-build, fail if not built"
    )

    args = parser.parse_args()

    # Determine how to create benchmark
    if args.exe:
        bench = Benchmark(executable_path=args.exe, warmup_runs=args.warmup)
    elif args.benchmark:
        bench = Benchmark(
            benchmark_name=args.benchmark, auto_build=not args.no_build,
            warmup_runs=args.warmup
        )
    else:
        parser.error("Must specify either benchmark name or --exe path")

    # Thread sweep
    if args.thread_sweep:
        fixed_params = {}
        for param in ['ring_dim', 'mult_depth', 'scale_mod_size']:
            value = getattr(args, param.replace('-', '_'))
            if value is not None:
                fixed_params[param] = value

        if args.check_security:
            fixed_params['check_security'] = True

        results = bench.thread_sweep(
            args.thread_sweep, runs=args.runs, fixed_params=fixed_params
        )
        bench.compare_results(results, 'threads')

        if args.json:
            with open(args.json, 'w') as f:
                json.dump([asdict(r) for r in results], f, indent=2)
            print(f"\n💾 Results saved to {args.json}")

    # Parameter sweep
    elif args.sweep:
        param_name = args.sweep[0]
        values = []
        for v in args.sweep[1:]:
            try:
                values.append(int(v))
            except ValueError:
                values.append(v)

        fixed_params = {}
        for param in ['ring_dim', 'mult_depth', 'scale_mod_size', 'threads']:
            value = getattr(args, param.replace('-', '_'))
            if value is not None and param.replace('_', '-') != param_name:
                fixed_params[param] = value

        if args.check_security:
            fixed_params['check_security'] = True

        results = bench.parameter_sweep(
            param_name.replace('-', '_'), values, runs=args.runs,
            fixed_params=fixed_params
        )
        bench.compare_results(results, param_name)

        if args.json:
            with open(args.json, 'w') as f:
                json.dump([asdict(r) for r in results], f, indent=2)
            print(f"\n💾 Results saved to {args.json}")

    # Single benchmark
    else:
        params = {}
        for param in ['ring_dim', 'mult_depth', 'scale_mod_size', 'threads']:
            value = getattr(args, param.replace('-', '_'))
            if value is not None:
                params[param] = value

        if args.check_security:
            params['check_security'] = True

        result = bench.run(runs=args.runs, **params)
        result.print_summary()

        if args.json:
            with open(args.json, 'w') as f:
                f.write(result.to_json())
            print(f"\n💾 Results saved to {args.json}")


if __name__ == "__main__":
    main()