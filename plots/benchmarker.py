#!/usr/bin/env python3
"""
Performance benchmarking framework for measuring execution metrics.

Provides comprehensive performance analysis including:
- Execution latency measurements
- DRAM traffic analysis
- Integer operation counting via Intel PIN
"""

import json
import re
import os
import subprocess
import time
from pathlib import Path
from statistics import mean, stdev


class Benchmarker:
    """
    A comprehensive benchmarking suite for performance analysis.
    
    Handles building, execution, and measurement of various performance metrics
    including timing, memory traffic, and instruction counts.
    """
    
    def __init__(self, debug=False):
        """Initialize the benchmarker with debug control."""
        self._debug = debug
        self._setup_paths()
        self._setup_default_config()
    
    def _setup_paths(self):
        """Configure all necessary paths for benchmarking tools."""
        current_file = Path(__file__).resolve()
        if current_file.parent.name == "plots":
            self.repo_root = current_file.parent.parent
        else:
            self.repo_root = current_file.parent
        
        self.build_dir = self.repo_root / "build"
        self.pin_path = Path("/opt/intel/pin/pin")
        self.pintool_path = Path("/opt/profiling-tools/lib/pintool.so")
    
    def _setup_default_config(self):
        """Set up the default configuration for all benchmarks."""
        self.base_config = {
            "ring_dim": 8192,
            "num_limbs": 2,
            "num_digits": 1,
            "matrix_dim": 128,
            "threads": 1,
            "timing_runs": 5,
            "check_security": False,
            "build": True,
            "debug": False,
        }
    
    def build(self, benchmark, clean=False):
        """
        Build a benchmark executable from source.
        
        Args:
            benchmark: Name of the benchmark to build
            clean: If True, force a clean rebuild
            
        Returns:
            Path to the built executable
        """
        self.build_dir.mkdir(exist_ok=True)
        
        # Optionally remove existing binary
        if clean:
            binary_path = self.build_dir / benchmark
            if binary_path.exists():
                binary_path.unlink()
        
        subprocess.run(
            [
                "cmake",
                "-S", str(self.repo_root),
                "-B", str(self.build_dir),
                f"-DBENCH_SOURCE=examples/{benchmark}.cpp",
                "-DCMAKE_BUILD_TYPE=Release"
            ],
            check=True,
            capture_output=not self._debug
        )
        
        # Add --clean-first to force rebuild
        build_cmd = [
            "cmake",
            "--build", str(self.build_dir),
            "-j", str(os.cpu_count())
        ]
        if clean:
            build_cmd.insert(2, "--clean-first")
        
        subprocess.run(build_cmd, check=True, capture_output=not self._debug)
        
        return self.build_dir / benchmark
    
    def measure_latency(self, target, args, runs=3):
        """
        Measure execution time over multiple runs.
        
        Args:
            target: Path to the executable
            args: Command line arguments
            runs: Number of timing runs to perform
            
        Returns:
            Tuple of (mean_time, standard_deviation) or (None, None) on failure
        """
        cmd = [str(target), *args, "--measure=latency"]
        
        times = []
        for _ in range(runs):
            start = time.perf_counter()
            result = subprocess.run(cmd, capture_output=not self._debug)
            
            if result.returncode == 0:
                elapsed = time.perf_counter() - start
                times.append(elapsed)
        
        if not times:
            return (None, None)
            
        return (mean(times), stdev(times) if len(times) > 1 else 0.0)
    
    def measure_dram(self, target, args):
        """
        Measure DRAM traffic during execution.
        
        Args:
            target: Path to the executable
            args: Command line arguments
            
        Returns:
            Dictionary with READ/WRITE/TOTAL bytes, or None on failure
        """
        cmd = ["sudo", "-n", str(target), *args, "--measure=dram"]
        
        result = subprocess.run(cmd, capture_output=True, text=True)
        
        if result.returncode != 0:
            return None
        
        data = {}
        for line in result.stdout.split('\n'):
            if 'DRAM_' in line and '=' in line:
                key, value = line.split('=', 1)
                data[key] = int(value)
        
        return data if data else None
    
    def measure_opcounts(self, target, args):
        """
        Measure integer operations using Intel PIN instrumentation.
        
        Args:
            target: Path to the executable
            args: Command line arguments
            
        Returns:
            Dictionary with operation counts, or None on failure
        """
        cmd = [
            "sudo", "-n",
            str(self.pin_path),
            "-t", str(self.pintool_path),
            "--",
            str(target),
            *args,
            "--measure=pin"
        ]
        
        result = subprocess.run(cmd, capture_output=True, text=True)
        
        if result.returncode != 0:
            return None
        
        json_match = re.search(r'\{[^}]+\}', result.stdout)
        if json_match:
            try:
                return json.loads(json_match.group())
            except (json.JSONDecodeError, ValueError):
                pass
        
        return None
    
    def run(self, benchmark, **kwargs):
        """
        Execute a complete benchmark with all measurements.
        
        Args:
            benchmark: Name of the benchmark to run
            **kwargs: Override parameters for this run
            
        Returns:
            Dictionary containing all measurement results and computed metrics
        """
        params = self.base_config.copy()
        params.update(kwargs)
        params["debug"] = self._debug
            
        # Validate ring dimension
        if params["ring_dim"] < 16:
            raise ValueError(f"Ring dimension must be at least 16, got {params['ring_dim']}")
        
        if params["build"]:
            clean = params.get("clean_build", False)
            target = self.build(benchmark, clean=clean)
        else:
            target = self.build_dir / benchmark
        
        args = self._prepare_arguments(params)
        
        latency = self.measure_latency(target, args, params["timing_runs"])
        dram = self.measure_dram(target, args)
        opcounts = self.measure_opcounts(target, args)
        
        success = latency[0] is not None
        
        ai = None
        if dram and opcounts:
            total_bytes = dram.get("DRAM_TOTAL_BYTES", 0)
            total_ops = opcounts.get("total", 0)
            if total_bytes > 0:
                ai = total_ops / total_bytes
        
        return {
            "benchmark": benchmark,
            "parameters": params,
            "success": success,
            "latency": latency,
            "dram": dram,
            "opcounts": opcounts,
            "ai": ai
        }
    
    def _prepare_arguments(self, params):
        """
        Convert parameter dictionary to command line arguments.
        
        Args:
            params: Dictionary of parameters
            
        Returns:
            List of command line argument strings
        """
        skip_keys = {"timing_runs", "build", "num_limbs", "clean_build"}
        
        args = []
        
        if "num_limbs" in params:
            mult_depth = params["num_limbs"] - 1
            args.append(f"--mult-depth={mult_depth}")
        
        for key, value in params.items():
            if key not in skip_keys and not key.startswith("_"):
                arg_name = key.replace("_", "-")
                if isinstance(value, bool):
                    args.append(f"--{arg_name}={'true' if value else 'false'}")
                else:
                    args.append(f"--{arg_name}={value}")
        
        return args