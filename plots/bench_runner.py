#!/usr/bin/env python3
"""
bench_runner.py - Benchmark runner that integrates with PIN tool
Works with or without PIN instrumentation
"""

import subprocess
import re
import os
from pathlib import Path
from typing import Dict, Any, Optional
from dataclasses import dataclass


@dataclass
class BenchmarkResult:
    """Results from a benchmark run"""
    integer_ops: Optional[int] = None
    dram_read_bytes: Optional[int] = None
    dram_write_bytes: Optional[int] = None
    dram_total_bytes: Optional[int] = None
    arithmetic_intensity: Optional[float] = None
    has_pin_data: bool = False
    has_dram_data: bool = False
    stdout: str = ""
    stderr: str = ""
    returncode: int = 0
    
    def __str__(self):
        lines = []
        if self.has_pin_data:
            lines.append(f"Integer ops: {self.integer_ops:,}")
        else:
            lines.append("Integer ops: Not measured (PIN not available)")
            
        if self.has_dram_data:
            lines.append(f"DRAM total: {self._format_bytes(self.dram_total_bytes)}")
            lines.append(f"  Read:  {self._format_bytes(self.dram_read_bytes)}")
            lines.append(f"  Write: {self._format_bytes(self.dram_write_bytes)}")
        else:
            lines.append("DRAM traffic: Not measured")
            
        if self.arithmetic_intensity is not None:
            lines.append(f"Arithmetic intensity: {self.arithmetic_intensity:.6f} ops/byte")
            
        return "\n".join(lines)
    
    def _format_bytes(self, bytes_val):
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


class BenchmarkRunner:
    """Runner for OpenFHE benchmarks with optional PIN instrumentation"""
    
    def __init__(self, repo_path: str = ".", build_dir: str = "build", use_pin: bool = False):
        self.repo_path = Path(repo_path).expanduser().resolve()
        self.build_dir = self.repo_path / build_dir
        self.use_pin = use_pin
        self.pin_path = "/opt/intel/pin/pin"
        self.pintool_path = "/opt/intel/pin/profiling-tools/pintool.so"
        
        if self.use_pin:
            if not Path(self.pin_path).exists():
                print(f"Warning: PIN not found at {self.pin_path}, disabling PIN")
                self.use_pin = False
            elif not Path(self.pintool_path).exists():
                print(f"Warning: PIN tool not found at {self.pintool_path}, disabling PIN")
                self.use_pin = False
    
    def _to_cli_args(self, params: Dict[str, Any]) -> list[str]:
        """Convert parameter dict to command-line arguments"""
        args = []
        for key, value in params.items():
            if value is None:
                continue
            
            cli_key = key.replace("_", "-")
            
            if isinstance(value, bool):
                value = "true" if value else "false"
            args.append(f"--{cli_key}={value}")
        
        return args
    
    def build(self, target: str, force_rebuild: bool = False) -> Path:
        """Build the specified target"""
        source_file = self.repo_path / "examples" / f"{target}.cpp"
        if not source_file.exists():
            raise ValueError(f"Source file not found: {source_file}")
        
        exe_path = self.build_dir / target
        if not force_rebuild and exe_path.exists():
            print(f"Executable already exists: {exe_path}")
            return exe_path
        
        print(f"Building {target}...")
        
        self.build_dir.mkdir(parents=True, exist_ok=True)
        
        # Configure with CMake
        result = subprocess.run(
            ["cmake", "-S", str(self.repo_path), "-B", str(self.build_dir),
             f"-DBENCH_SOURCE=examples/{target}.cpp"],
            cwd=self.repo_path,
            capture_output=True,
            text=True
        )
        
        if result.returncode != 0:
            print(f"CMake configuration failed:\n{result.stderr}")
            raise RuntimeError("CMake configuration failed")
        
        # Build
        result = subprocess.run(
            ["cmake", "--build", str(self.build_dir)],
            cwd=self.repo_path,
            capture_output=True,
            text=True
        )
        
        if result.returncode != 0:
            print(f"Build failed:\n{result.stderr}")
            raise RuntimeError("Build failed")
        
        print(f"Successfully built: {exe_path}")
        return exe_path
    
    def run(self, target: str, params: Optional[Dict[str, Any]] = None,
            rebuild: bool = False) -> BenchmarkResult:
        """Run a benchmark with optional PIN instrumentation"""
        
        # Build if necessary
        exe_path = self.build(target, force_rebuild=rebuild)
        
        # Clean up old logs
        logs_dir = self.repo_path / "logs"
        logs_dir.mkdir(exist_ok=True)
        for logfile in ["int_counts.out", "dram_counts.out"]:
            (logs_dir / logfile).unlink(missing_ok=True)
        
        # Clean up any leftover temp directories
        subprocess.run(["rm", "-rf"] + list(Path("/tmp").glob("openfhe_bench_*")), 
                      capture_output=True)
        
        # Prepare command
        exe_args = self._to_cli_args(params) if params else []
        
        if self.use_pin:
            # Run with PIN instrumentation
            cmd = ["sudo", self.pin_path, "-t", self.pintool_path, "-quiet", "--", 
                   str(exe_path)] + exe_args
            print(f"Running {target} with PIN instrumentation...")
        else:
            # Run without PIN
            cmd = ["sudo", str(exe_path)] + exe_args
            print(f"Running {target} (without PIN)...")
        
        if params:
            print(f"Parameters: {params}")
        
        # Run the benchmark
        result = subprocess.run(
            cmd,
            cwd=self.repo_path,
            capture_output=True,
            text=True
        )
        
        # Parse output
        output = BenchmarkResult(
            stdout=result.stdout,
            stderr=result.stderr,
            returncode=result.returncode
        )
        
        # Parse the structured output from C++
        if "=== PROFILING_METRICS ===" in result.stdout:
            metrics_section = result.stdout.split("=== PROFILING_METRICS ===")[1]
            metrics_section = metrics_section.split("=== END_PROFILING_METRICS ===")[0]
            
            for line in metrics_section.strip().split('\n'):
                if '=' in line:
                    key, value = line.split('=', 1)
                    if key == "INTEGER_OPS":
                        output.integer_ops = int(value)
                    elif key == "DRAM_READ_BYTES":
                        output.dram_read_bytes = int(value)
                    elif key == "DRAM_WRITE_BYTES":
                        output.dram_write_bytes = int(value)
                    elif key == "DRAM_TOTAL_BYTES":
                        output.dram_total_bytes = int(value)
                    elif key == "ARITHMETIC_INTENSITY":
                        output.arithmetic_intensity = float(value)
                    elif key == "HAS_PIN_DATA":
                        output.has_pin_data = (value.lower() == "true")
                    elif key == "HAS_DRAM_DATA":
                        output.has_dram_data = (value.lower() == "true")
        
        # Clean up temp directories
        subprocess.run(["rm", "-rf"] + list(Path("/tmp").glob("openfhe_bench_*")), 
                      capture_output=True)
        
        return output
    
    def parameter_sweep(self, target: str, 
                       param_sets: list[Dict[str, Any]]) -> list[Dict[str, Any]]:
        """Run multiple parameter configurations"""
        results = []
        
        # Build once
        self.build(target)
        
        for i, params in enumerate(param_sets, 1):
            print(f"\n--- Run {i}/{len(param_sets)} ---")
            
            result = self.run(target, params, rebuild=False)
            
            results.append({
                "params": params,
                "integer_ops": result.integer_ops,
                "dram_total_bytes": result.dram_total_bytes,
                "arithmetic_intensity": result.arithmetic_intensity,
                "has_pin_data": result.has_pin_data,
                "has_dram_data": result.has_dram_data
            })
        
        return results


# Example usage
def example_with_pin():
    """Run with PIN instrumentation"""
    runner = BenchmarkRunner(".", use_pin=True)
    
    result = runner.run(
        "addition",
        params={
            "mult_depth": 15,
            "scale_mod_size": 45,
            "ring_dim": 32768
        }
    )
    
    print("\n=== Results ===")
    print(result)
    
    return result


def example_without_pin():
    """Run without PIN (DRAM only)"""
    runner = BenchmarkRunner(".", use_pin=False)
    
    result = runner.run(
        "addition",
        params={
            "mult_depth": 15,
            "scale_mod_size": 45,
            "ring_dim": 32768
        }
    )
    
    print("\n=== Results (DRAM only) ===")
    print(result)
    
    return result


def example_parameter_sweep():
    """Parameter sweep with PIN if available"""
    runner = BenchmarkRunner(".", use_pin=True)  # Will auto-disable if PIN not found
    
    param_sets = [
        {"mult_depth": 10, "scale_mod_size": 40, "ring_dim": 16384},
        {"mult_depth": 15, "scale_mod_size": 45, "ring_dim": 32768},
        {"mult_depth": 19, "scale_mod_size": 50, "ring_dim": 65536},
    ]
    
    results = runner.parameter_sweep("addition", param_sets)
    
    print("\n=== Parameter Sweep Results ===")
    print("| Ring Dim | Mult Depth | Has PIN | Has DRAM | AI (ops/byte) |")
    print("|----------|------------|---------|----------|---------------|")
    
    for result in results:
        params = result["params"]
        ai = result.get("arithmetic_intensity", 0)
        has_pin = "✓" if result["has_pin_data"] else "✗"
        has_dram = "✓" if result["has_dram_data"] else "✗"
        
        print(f"| {params['ring_dim']:8} | {params['mult_depth']:10} | "
              f"{has_pin:^7} | {has_dram:^8} | {ai:13.6f} |")
    
    return results


if __name__ == "__main__":
    import sys
    
    print("OpenFHE Benchmark Runner")
    print("========================\n")
    
    if len(sys.argv) > 1:
        if sys.argv[1] == "pin":
            example_with_pin()
        elif sys.argv[1] == "nopin":
            example_without_pin()
        elif sys.argv[1] == "sweep":
            example_parameter_sweep()
    else:
        print("Usage:")
        print("  python bench_runner.py pin    # Run with PIN instrumentation")
        print("  python bench_runner.py nopin  # Run without PIN (DRAM only)")
        print("  python bench_runner.py sweep  # Parameter sweep")
        print("\nTrying to run with PIN...")
        example_with_pin()