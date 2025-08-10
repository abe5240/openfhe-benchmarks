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
    
    # ============================================================================
    # Initialization
    # ============================================================================
    
    def __init__(self):
        """Initialize the benchmarker with paths and default configuration."""
        self._setup_paths()
        self._setup_default_config()
    
    def _setup_paths(self):
        """Configure all necessary paths for benchmarking tools."""
        # Determine repository root based on script location
        current_file = Path(__file__).resolve()
        if current_file.parent.name == "plots":
            self.repo_root = current_file.parent.parent
        else:
            self.repo_root = current_file.parent
        
        # Build and tool paths
        self.build_dir = self.repo_root / "build"
        self.pin_path = Path("/opt/intel/pin/pin")
        self.pintool_path = Path("/opt/profiling-tools/lib/pintool.so")
    
    def _setup_default_config(self):
        """Set up the default configuration for all benchmarks."""
        self.base_config = {
            # Core parameters
            "ring_dim": 8192,
            "mult_depth": 1,
            "num_digits": 2,
            "threads": 1,
            
            # Security and verification
            "check_security": False,
            "skip_verify": True,
            
            # Execution settings
            "timing_runs": 3,
            "build": True,
            "quiet": True,
            
            # Additional parameters (may not be used by all benchmarks)
            "matrix_dim": 128,
        }
    
    # ============================================================================
    # Build Management
    # ============================================================================
    
    def build(self, benchmark):
        """
        Build a benchmark executable from source.
        
        Args:
            benchmark: Name of the benchmark to build
            
        Returns:
            Path to the built executable
        """
        self.build_dir.mkdir(exist_ok=True)
        
        # Configure with CMake
        subprocess.run(
            [
                "cmake",
                "-S", str(self.repo_root),
                "-B", str(self.build_dir),
                f"-DBENCH_SOURCE=examples/{benchmark}.cpp",
                "-DCMAKE_BUILD_TYPE=Release"
            ],
            check=True,
            capture_output=True
        )
        
        # Build with all available cores
        subprocess.run(
            [
                "cmake",
                "--build", str(self.build_dir),
                "-j", str(os.cpu_count())
            ],
            check=True,
            capture_output=True
        )
        
        return self.build_dir / benchmark
    
    # ============================================================================
    # Measurement Functions
    # ============================================================================
    
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
        cmd = [
            str(target),
            *args,
            "--measure=none",
            "--skip-verify=true"
        ]
        
        times = []
        for _ in range(runs):
            start = time.perf_counter()
            # Don't capture output - let the quiet flag control visibility
            result = subprocess.run(cmd, stderr=subprocess.DEVNULL)
            
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
        cmd = [
            "sudo", "-n",
            str(target),
            *args,
            "--measure=dram",
            "--skip-verify=true"
        ]
        
        result = subprocess.run(cmd, capture_output=True, text=True)
        
        if result.returncode != 0:
            return None
        
        # Parse DRAM statistics from output
        data = {}
        for line in result.stdout.split('\n'):
            if 'DRAM_' in line and '=' in line:
                key, value = line.split('=', 1)
                data[key] = int(value)
        
        return data if data else None
    
    def measure_opcounts(self, target, args):
        """
        Measure integer operations using Intel PIN instrumentation.
        """
        cmd = [
            "sudo", "-n",
            str(self.pin_path),
            "-t", str(self.pintool_path),
            "--",
            str(target),
            *args,
            "--measure=pin",
            "--skip-verify=true"
        ]
        
        result = subprocess.run(cmd, capture_output=True, text=True)
        
        if result.returncode != 0:
            return None
        
        # Extract JSON from output (it might have other text around it)
        json_match = re.search(r'\{[^}]+\}', result.stdout)
        if json_match:
            try:
                return json.loads(json_match.group())
            except (json.JSONDecodeError, ValueError):
                pass
        
        return None
    
    # ============================================================================
    # Main Execution
    # ============================================================================
    
    def run(self, benchmark, **kwargs):
        """
        Execute a complete benchmark with all measurements.
        
        Args:
            benchmark: Name of the benchmark to run
            **kwargs: Override parameters for this run
            
        Returns:
            Dictionary containing all measurement results and computed metrics
        """
        # Merge base configuration with overrides
        params = self.base_config.copy()
        params.update(kwargs)
        
        # Build or locate executable
        if params["build"]:
            target = self.build(benchmark)
        else:
            target = self.build_dir / benchmark
        
        # Convert parameters to command line arguments
        args = self._prepare_arguments(params)
        
        # Perform all measurements
        results = {
            "benchmark": benchmark,
            "parameters": params,
            "latency": self.measure_latency(
                target, args, params["timing_runs"]
            ),
            "dram": self.measure_dram(target, args),
            "opcounts": self.measure_opcounts(target, args)
        }
        
        # Calculate arithmetic intensity if data is available
        self._calculate_arithmetic_intensity(results)
        
        return results
    
    # ============================================================================
    # Helper Methods
    # ============================================================================
    
    def _prepare_arguments(self, params):
        """
        Convert parameter dictionary to command line arguments.
        
        Args:
            params: Dictionary of parameters
            
        Returns:
            List of command line argument strings
        """
        # Skip internal parameters (those starting with _) and control flags
        skip_keys = {"timing_runs", "build"}
        
        args = []
        for key, value in params.items():
            if key not in skip_keys and not key.startswith("_"):
                arg_name = key.replace("_", "-")
                args.append(f"--{arg_name}={value}")
        
        return args
    
    def _calculate_arithmetic_intensity(self, results):
        """
        Calculate arithmetic intensity (ops/byte) if data is available.
        
        Args:
            results: Dictionary to update with AI metric
        """
        if not (results["dram"] and results["opcounts"]):
            return
        
        total_bytes = results["dram"].get("DRAM_TOTAL_BYTES", 0)
        total_ops = results["opcounts"].get("total", 0)
        
        if total_bytes > 0:
            results["ai"] = total_ops / total_bytes