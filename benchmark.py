#!/usr/bin/env python3
"""
benchmark.py - OpenFHE Benchmark Runner with Arithmetic Intensity Measurement
"""

import subprocess
import time
import re
import statistics
import json
import sys
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
            
        # Calculate arithmetic intensity (ops per byte)
        if self.integer_ops and self.dram_total_bytes:
            self.arithmetic_intensity = self.integer_ops / self.dram_total_bytes
            
        # Calculate throughput (billion ops per second)
        if self.integer_ops and self.runtime_sec > 0:
            self.throughput_gops = (self.integer_ops / 1e9) / self.runtime_sec
            
        # Calculate memory bandwidth
        if self.dram_total_bytes and self.runtime_sec > 0:
            self.bandwidth_gb_sec = (self.dram_total_bytes / (1 << 30)) / self.runtime_sec
    
    def to_json(self) -> str:
        """Export results as JSON"""
        return json.dumps(asdict(self), indent=2)
    
    def print_summary(self):
        """Print human-readable summary"""
        print("\n" + "="*60)
        print("BENCHMARK RESULTS")
        print("="*60)
        
        if self.parameters:
            print("\nParameters:")
            for key, value in self.parameters.items():
                print(f"  {key}: {value}")
        
        print(f"\nPerformance:")
        print(f"  Runtime: {self.runtime_sec:.3f} ± {self.runtime_stdev:.3f} seconds")
        
        if self.dram_total_bytes:
            print(f"\nMemory Traffic:")
            print(f"  Read:  {self._format_bytes(self.dram_read_bytes)}")
            print(f"  Write: {self._format_bytes(self.dram_write_bytes)}")
            print(f"  Total: {self._format_bytes(self.dram_total_bytes)}")
        
        if self.integer_ops:
            print(f"\nComputation:")
            print(f"  Total operations: {self.integer_ops:,}")
            if self.ops_breakdown:
                for op, count in sorted(self.ops_breakdown.items()):
                    percentage = (count / self.integer_ops) * 100
                    print(f"    {op}: {count:,} ({percentage:.1f}%)")
        
        print(f"\nEfficiency Metrics:")
        if self.arithmetic_intensity is not None:
            print(f"  Arithmetic Intensity: {self.arithmetic_intensity:.6f} ops/byte")
        if self.throughput_gops:
            print(f"  Throughput: {self.throughput_gops:.3f} Gops/sec")
        if self.bandwidth_gb_sec:
            print(f"  Memory Bandwidth: {self.bandwidth_gb_sec:.2f} GB/sec")
            
        print("="*60)
    
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


class Benchmark:
    """OpenFHE benchmark runner"""
    
    def __init__(self, executable: str, 
                 pin_path: str = "/opt/intel/pin/pin",
                 pintool_path: str = "/opt/profiling-tools/lib/pintool.so"):
        self.executable = Path(executable).resolve()
        if not self.executable.exists():
            raise FileNotFoundError(f"Executable not found: {executable}")
            
        self.pin_path = pin_path
        self.pintool_path = pintool_path
        
        # Verify tools are available
        self._verify_tools()
    
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
    
    def run(self, **params) -> BenchmarkResult:
        """
        Run benchmark with specified parameters
        
        Args:
            **params: Benchmark parameters (e.g., ring_dim=8192, mult_depth=10)
        
        Returns:
            BenchmarkResult with all measurements
        """
        # Convert parameters to command-line arguments
        args = []
        for key, value in params.items():
            param_name = key.replace('_', '-')
            args.append(f"--{param_name}={value}")
        
        result = BenchmarkResult(parameters=params)
        
        # Measure performance
        print(f"\n🔬 Benchmarking {self.executable.name}")
        if params:
            print(f"   Parameters: {params}")
        
        perf_data = self._measure_performance(args)
        if perf_data:
            result.runtime_sec = perf_data['mean']
            result.runtime_stdev = perf_data['stdev']
        
        # Measure DRAM traffic
        dram_data = self._measure_dram(args)
        if dram_data:
            result.dram_read_bytes = dram_data.get('read')
            result.dram_write_bytes = dram_data.get('write')
            result.dram_total_bytes = dram_data.get('total')
        
        # Count operations (if PIN available)
        if self.pin_available:
            ops_data = self._count_operations(args)
            if ops_data:
                result.integer_ops = ops_data['total']
                result.ops_breakdown = ops_data['breakdown']
        
        # Recalculate derived metrics
        result.__post_init__()
        
        return result
    
    def _measure_performance(self, args: List[str], runs: int = 3) -> Dict[str, float]:
        """Measure runtime performance"""
        print("  ⏱️  Measuring performance...", end=" ")
        
        times = []
        cmd = [str(self.executable)] + args + ["--measure=none", "--quiet=true", "--skip-verify=true"]
        
        for _ in range(runs):
            start = time.perf_counter()
            try:
                subprocess.run(cmd, capture_output=True, check=True)
                times.append(time.perf_counter() - start)
            except subprocess.CalledProcessError:
                return None
        
        if times:
            result = {
                'mean': statistics.mean(times),
                'stdev': statistics.stdev(times) if len(times) > 1 else 0.0
            }
            print(f"{result['mean']:.3f}s")
            return result
        return None
    
    def _measure_dram(self, args: List[str]) -> Dict[str, int]:
        """Measure DRAM traffic using hardware counters"""
        print("  💾 Measuring memory traffic...", end=" ")
        
        cmd = ["sudo", str(self.executable)] + args + ["--measure=dram", "--quiet=true", "--skip-verify=true"]
        
        try:
            result = subprocess.run(cmd, capture_output=True, check=True, text=True)
            
            dram_data = {}
            for line in result.stdout.split('\n'):
                if line.startswith("DRAM_READ_BYTES="):
                    dram_data['read'] = int(line.split('=')[1])
                elif line.startswith("DRAM_WRITE_BYTES="):
                    dram_data['write'] = int(line.split('=')[1])
                elif line.startswith("DRAM_TOTAL_BYTES="):
                    dram_data['total'] = int(line.split('=')[1])
            
            if dram_data.get('total'):
                print(f"{dram_data['total'] / (1<<20):.1f} MiB")
                return dram_data
        except subprocess.CalledProcessError:
            pass
        
        print("N/A")
        return None
    
    def _count_operations(self, args: List[str]) -> Dict[str, Any]:
        """Count operations using PIN instrumentation"""
        print("  🔢 Counting operations...", end=" ")
        
        cmd = ["sudo", self.pin_path, "-t", self.pintool_path, "--",
               str(self.executable)] + args + ["--measure=pin", "--quiet=true", "--skip-verify=true"]
        
        try:
            result = subprocess.run(cmd, capture_output=True, check=True, text=True)
            
            ops_data = {'total': 0, 'breakdown': {}}
            
            for line in result.stderr.split('\n'):
                if "TOTAL:" in line:
                    match = re.search(r'TOTAL:\s*(\d+)', line)
                    if match:
                        ops_data['total'] = int(match.group(1))
                elif "ADD:" in line:
                    match = re.search(r'ADD:\s*(\d+)', line)
                    if match:
                        ops_data['breakdown']['ADD'] = int(match.group(1))
                elif "SUB:" in line:
                    match = re.search(r'SUB:\s*(\d+)', line)
                    if match:
                        ops_data['breakdown']['SUB'] = int(match.group(1))
                elif "MUL:" in line:
                    match = re.search(r'MUL:\s*(\d+)', line)
                    if match:
                        ops_data['breakdown']['MUL'] = int(match.group(1))
                elif "DIV:" in line:
                    match = re.search(r'DIV:\s*(\d+)', line)
                    if match:
                        ops_data['breakdown']['DIV'] = int(match.group(1))
            
            if ops_data['total'] > 0:
                print(f"{ops_data['total']:,} ops")
                return ops_data
        except subprocess.CalledProcessError:
            pass
        
        print("N/A")
        return None
    
    def parameter_sweep(self, param_name: str, values: List[Any], 
                       fixed_params: Dict[str, Any] = None) -> List[BenchmarkResult]:
        """
        Run benchmark across multiple parameter values
        
        Args:
            param_name: Parameter to vary (e.g., 'ring_dim')
            values: List of values to test
            fixed_params: Other parameters to keep constant
        
        Returns:
            List of BenchmarkResult objects
        """
        results = []
        fixed_params = fixed_params or {}
        
        print(f"\n📊 Parameter sweep: {param_name}")
        print(f"   Values: {values}")
        if fixed_params:
            print(f"   Fixed: {fixed_params}")
        
        for value in values:
            params = {**fixed_params, param_name: value}
            result = self.run(**params)
            results.append(result)
        
        return results
    
    def compare_results(self, results: List[BenchmarkResult], param_name: str = None):
        """Print comparison table of multiple results"""
        print("\n" + "="*80)
        print("COMPARISON TABLE")
        print("="*80)
        
        # Header
        headers = ["Parameters", "Runtime (s)", "DRAM (MiB)", "Ops (M)", "AI (ops/B)", "Throughput"]
        print(f"{'Parameters':<20} {'Runtime':<12} {'DRAM':<12} {'Ops':<12} {'AI':<12} {'Gops/s':<10}")
        print("-"*80)
        
        # Rows
        for result in results:
            # Format parameters
            if param_name and result.parameters:
                param_str = f"{param_name}={result.parameters.get(param_name, 'N/A')}"
            else:
                param_str = str(result.parameters)[:20]
            
            # Format values
            runtime = f"{result.runtime_sec:.3f}" if result.runtime_sec else "N/A"
            dram = f"{result.dram_total_bytes/(1<<20):.1f}" if result.dram_total_bytes else "N/A"
            ops = f"{result.integer_ops/1e6:.1f}" if result.integer_ops else "N/A"
            ai = f"{result.arithmetic_intensity:.4f}" if result.arithmetic_intensity else "N/A"
            throughput = f"{result.throughput_gops:.2f}" if result.throughput_gops else "N/A"
            
            print(f"{param_str:<20} {runtime:<12} {dram:<12} {ops:<12} {ai:<12} {throughput:<10}")
        
        print("="*80)


def main():
    """Command-line interface"""
    import argparse
    
    parser = argparse.ArgumentParser(
        description="OpenFHE Benchmark Runner",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # Basic benchmark
  python3 benchmark.py ./build/addition --ring-dim=8192
  
  # Multiple parameters
  python3 benchmark.py ./build/addition --ring-dim=16384 --mult-depth=10
  
  # Parameter sweep
  python3 benchmark.py ./build/addition --sweep ring-dim 4096 8192 16384
  
  # Save results
  python3 benchmark.py ./build/addition --ring-dim=8192 --json results.json
        """
    )
    
    parser.add_argument("executable", help="Path to benchmark executable")
    parser.add_argument("--ring-dim", type=int, help="Ring dimension")
    parser.add_argument("--mult-depth", type=int, help="Multiplicative depth")
    parser.add_argument("--scale-mod-size", type=int, help="Scaling modulus size")
    parser.add_argument("--check-security", type=bool, help="Enable security check")
    
    parser.add_argument("--sweep", nargs='+', help="Parameter sweep: name value1 value2 ...")
    parser.add_argument("--json", help="Save results to JSON file")
    
    args = parser.parse_args()
    
    # Create benchmark runner
    try:
        bench = Benchmark(args.executable)
    except FileNotFoundError as e:
        print(f"Error: {e}")
        sys.exit(1)
    
    # Handle parameter sweep
    if args.sweep:
        param_name = args.sweep[0]
        values = [int(v) if v.isdigit() else v for v in args.sweep[1:]]
        
        # Fixed parameters
        fixed_params = {}
        for param in ['ring_dim', 'mult_depth', 'scale_mod_size', 'check_security']:
            value = getattr(args, param.replace('-', '_'))
            if value is not None and param.replace('_', '-') != param_name:
                fixed_params[param] = value
        
        results = bench.parameter_sweep(param_name.replace('-', '_'), values, fixed_params)
        bench.compare_results(results, param_name)
        
        if args.json:
            with open(args.json, 'w') as f:
                json.dump([asdict(r) for r in results], f, indent=2)
            print(f"\n💾 Results saved to {args.json}")
    
    # Single benchmark
    else:
        params = {}
        for param in ['ring_dim', 'mult_depth', 'scale_mod_size', 'check_security']:
            value = getattr(args, param)
            if value is not None:
                params[param] = value
        
        result = bench.run(**params)
        result.print_summary()
        
        if args.json:
            with open(args.json, 'w') as f:
                f.write(result.to_json())
            print(f"\n💾 Results saved to {args.json}")


if __name__ == "__main__":
    main()