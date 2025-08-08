#!/usr/bin/env python3
"""
bench_runner.py - Benchmark runner with proper PIN integration
Parses PIN output from stderr to avoid timing issues
"""

import subprocess
import os
import re
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
            
        if self.arithmetic_intensity is not None and self.arithmetic_intensity > 0:
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
    """Runner for OpenFHE benchmarks with PIN instrumentation"""
    
    def __init__(self, repo_path: str = ".", build_dir: str = "build", use_pin: bool = False):
        self.repo_path = Path(repo_path).expanduser().resolve()
        self.build_dir = self.repo_path / build_dir
        self.use_pin = use_pin
        
        # Updated paths for the new organization
        self.pin_path = "/opt/intel/pin/pin"
        self.pintool_path = "/opt/profiling-tools/lib/pintool.so"
        
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
        
        exe_path = self.build(target, force_rebuild=rebuild)
        
        # Clean up old logs
        logs_dir = self.repo_path / "logs"
        logs_dir.mkdir(exist_ok=True)
        for logfile in ["int_counts.out", "dram_counts.out"]:
            (logs_dir / logfile).unlink(missing_ok=True)
        
        # Clean up temp directories
        subprocess.run(["rm", "-rf"] + list(Path("/tmp").glob("openfhe_bench_*")), 
                      capture_output=True)
        
        exe_args = self._to_cli_args(params) if params else []
        
        if self.use_pin:
            cmd = ["sudo", self.pin_path, "-t", self.pintool_path, "--", 
                   str(exe_path)] + exe_args
            print(f"Running {target} with PIN instrumentation...")
        else:
            cmd = ["sudo", str(exe_path)] + exe_args
            print(f"Running {target} (without PIN)...")
        
        if params:
            print(f"Parameters: {params}")
        
        result = subprocess.run(cmd, cwd=self.repo_path, capture_output=True, text=True)
        
        output = BenchmarkResult(
            stdout=result.stdout,
            stderr=result.stderr,
            returncode=result.returncode
        )
        
        # Parse PIN output from stderr (real-time output)
        if self.use_pin and "TOTAL:" in result.stderr:
            pin_match = re.search(r"TOTAL:\s*(\d+)", result.stderr)
            if pin_match:
                output.integer_ops = int(pin_match.group(1))
                output.has_pin_data = True
                print(f"  PIN counted: {output.integer_ops:,} operations")
        
        # Parse DRAM data from stdout
        if "DRAM_TOTAL_BYTES=" in result.stdout:
            dram_match = re.search(r"DRAM_READ_BYTES=(\d+)", result.stdout)
            if dram_match:
                output.dram_read_bytes = int(dram_match.group(1))
            
            dram_match = re.search(r"DRAM_WRITE_BYTES=(\d+)", result.stdout)
            if dram_match:
                output.dram_write_bytes = int(dram_match.group(1))
            
            dram_match = re.search(r"DRAM_TOTAL_BYTES=(\d+)", result.stdout)
            if dram_match:
                output.dram_total_bytes = int(dram_match.group(1))
                output.has_dram_data = True
                print(f"  DRAM traffic: {output._format_bytes(output.dram_total_bytes)}")
        
        # Calculate arithmetic intensity
        if output.has_pin_data and output.has_dram_data and output.dram_total_bytes > 0:
            output.arithmetic_intensity = output.integer_ops / output.dram_total_bytes
            print(f"  Arithmetic intensity: {output.arithmetic_intensity:.6f} ops/byte")
        
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


if __name__ == "__main__":
    import sys
    
    runner = BenchmarkRunner(".", use_pin=True)
    
    if len(sys.argv) > 1:
        if sys.argv[1] == "test":
            result = runner.run("addition", {"ring_dim": 8192})
            print("\n=== Test Results ===")
            print(result)
        elif sys.argv[1] == "sweep":
            # Parameter sweep
            results = runner.parameter_sweep("addition", [
                {"ring_dim": 4096},
                {"ring_dim": 8192},
                {"ring_dim": 16384},
            ])
            print("\n=== Sweep Results ===")
            print("Ring Dim | Int Ops    | DRAM      | AI (ops/byte)")
            print("---------|------------|-----------|---------------")
            for r in results:
                rd = r['params']['ring_dim']
                ops = r.get('integer_ops', 0)
                dram = r.get('dram_total_bytes', 0)
                ai = r.get('arithmetic_intensity', 0)
                dram_str = f"{dram/(1<<20):.1f} MiB" if dram else "N/A"
                print(f"{rd:8} | {ops:10,} | {dram_str:9} | {ai:.6f}")
    else:
        print("Usage:")
        print("  python bench_runner.py test   # Single test")
        print("  python bench_runner.py sweep  # Parameter sweep")