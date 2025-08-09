#!/usr/bin/env python3
import json
import os
import subprocess
import time
from pathlib import Path
from statistics import mean, stdev

class Benchmarker:
    def __init__(self):
        # If benchmarker.py is in plots/, go up one level to repo root
        current_file = Path(__file__).resolve()
        if current_file.parent.name == "plots":
            self.repo_root = current_file.parent.parent
        else:
            self.repo_root = current_file.parent
        self.build_dir = self.repo_root / "build"
        self.pin_path = Path("/opt/intel/pin/pin")
        self.pintool_path = Path("/opt/profiling-tools/lib/pintool.so")
    
    def build(self, benchmark):
        """Build a benchmark and return path to executable."""
        self.build_dir.mkdir(exist_ok=True)
        
        subprocess.run([
            "cmake", "-S", str(self.repo_root), "-B", str(self.build_dir),
            f"-DBENCH_SOURCE=examples/{benchmark}.cpp",
            "-DCMAKE_BUILD_TYPE=Release"
        ], check=True, capture_output=True)
        
        subprocess.run([
            "cmake", "--build", str(self.build_dir), "-j", str(os.cpu_count())
        ], check=True, capture_output=True)
        
        return self.build_dir / benchmark
    
    def measure_latency(self, target, args, runs=3):
        """Measure execution time. Returns (mean, stdev)."""
        cmd = [str(target)] + args + ["--measure=none", "--quiet=true", "--skip-verify=true"]
        
        times = []
        for _ in range(runs):
            start = time.perf_counter()
            if subprocess.run(cmd, capture_output=True).returncode == 0:
                times.append(time.perf_counter() - start)
        
        return (mean(times), stdev(times)) if times else (None, None)
    
    def measure_dram(self, target, args):
        """Measure DRAM traffic. Returns dict with READ/WRITE/TOTAL bytes."""
        cmd = ["sudo", "-n", str(target)] + args + ["--measure=dram", "--quiet=true", "--skip-verify=true"]
        result = subprocess.run(cmd, capture_output=True, text=True)
        
        if result.returncode != 0:
            return None
            
        data = {}
        for line in result.stdout.split('\n'):
            if 'DRAM_' in line and '=' in line:
                key, val = line.split('=', 1)
                data[key] = int(val)
        return data
    
    def measure_opcounts(self, target, args):
        """Measure integer operations using PIN. Returns dict with op counts."""
        cmd = ["sudo", "-n", str(self.pin_path), "-t", str(self.pintool_path), "--", str(target)] + \
              args + ["--measure=pin", "--quiet=true", "--skip-verify=true"]
        
        result = subprocess.run(cmd, capture_output=True, text=True)
        
        if result.returncode != 0:
            return None
            
        try:
            return json.loads(result.stdout.strip())
        except:
            return None
    
    def run(self, benchmark, **kwargs):
        """Run a benchmark with all measurements."""
        # Defaults
        params = {
            "ring_dim": 8192,
            "mult_depth": 1, 
            "num_digits": 3,
            "threads": 1,
            "timing_runs": 3,
            "build": True
        }
        params.update(kwargs)
        
        # Build if needed
        if params["build"]:
            target = self.build(benchmark)
        else:
            target = self.build_dir / benchmark
        
        # Prepare arguments
        args = [f"--{k.replace('_','-')}={v}" for k, v in params.items() 
                if k not in ["timing_runs", "build"] and not k.startswith("_")]
        
        # Measure
        results = {
            "benchmark": benchmark, 
            "parameters": params,
            "latency": self.measure_latency(target, args, params["timing_runs"]),
            "dram": self.measure_dram(target, args),
            "opcounts": self.measure_opcounts(target, args)
        }
        
        # Calculate AI if possible
        if results["dram"] and results["opcounts"]:
            total_bytes = results["dram"].get("DRAM_TOTAL_BYTES", 1)
            total_ops = results["opcounts"].get("total", 0)
            if total_bytes > 0:
                results["ai"] = total_ops / total_bytes
        
        return results