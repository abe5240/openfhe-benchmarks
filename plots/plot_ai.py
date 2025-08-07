#!/usr/bin/env python3
"""
plot_ai.py - Plot arithmetic intensity for various parameters
Usage: python3 plot_ai.py
"""

import subprocess
import re
import matplotlib.pyplot as plt
import numpy as np
import json
from datetime import datetime

def run_benchmark_with_args(mult_depth, ring_dim=65536):
    """Run benchmark with specific parameters using command line args."""
    try:
        # Build and run with arguments
        cmd = [
            "sudo", 
            "/opt/intel/pin/pin",
            "-t", "/opt/intel/pin/profiling-tools/pintool.so",
            "-quiet",
            "--",
            "../build/bench_mult_args",
            str(mult_depth),
            str(ring_dim)
        ]
        
        result = subprocess.run(
            cmd,
            cwd="..",
            capture_output=True,
            text=True,
            timeout=300
        )
        
        # Parse AI from output
        ai_match = re.search(r'AI \(ops/byte\)\s*:\s*([\d.]+)', result.stdout)
        if ai_match:
            return float(ai_match.group(1))
        
        # Also check the log file
        try:
            with open("../logs/dram_counts.out", "r") as f:
                data = {}
                for line in f:
                    key, val = line.strip().split('=')
                    data[key] = int(val)
            
            with open("../logs/int_counts.out", "r") as f:
                for line in f:
                    if "TOTAL" in line:
                        ops = int(line.split()[2])
                        ai = ops / data['DRAM_TOTAL_BYTES']
                        return ai
        except:
            pass
            
        return None
        
    except Exception as e:
        print(f"Error: {e}")
        return None

def plot_mult_depth():
    """Generate plot for multiplicative depth vs AI."""
    
    depths = list(range(1, 21))
    ai_values = []
    
    print("Benchmarking multiplicative depth...")
    print("Depth | AI (ops/byte)")
    print("-" * 30)
    
    for depth in depths:
        ai = run_benchmark_with_args(depth)
        if ai:
            ai_values.append(ai)
            print(f"{depth:5d} | {ai:.6f}")
        else:
            ai_values.append(np.nan)
            print(f"{depth:5d} | FAILED")
    
    # Save data
    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    data = {
        "parameter": "mult_depth",
        "values": depths,
        "ai": [a for a in ai_values if not np.isnan(a)],
        "ring_dim": 65536,
        "timestamp": timestamp
    }
    
    with open(f"data_mult_depth_{timestamp}.json", "w") as f:
        json.dump(data, f, indent=2)
    
    # Plot
    plt.figure(figsize=(10, 6))
    valid_x = [d for d, a in zip(depths, ai_values) if not np.isnan(a)]
    valid_y = [a for a in ai_values if not np.isnan(a)]
    
    plt.plot(valid_x, valid_y, 'b-o', linewidth=2, markersize=8)
    plt.xlabel('Multiplicative Depth', fontsize=12)
    plt.ylabel('Arithmetic Intensity (ops/byte)', fontsize=12)
    plt.title('AI vs Multiplicative Depth (Ring Dim = 65536, 128-bit secure)', fontsize=14)
    plt.grid(True, alpha=0.3)
    
    # Annotate min/max
    if valid_y:
        min_idx = valid_y.index(min(valid_y))
        max_idx = valid_y.index(max(valid_y))
        plt.annotate(f'Min: {min(valid_y):.3f}', 
                    xy=(valid_x[min_idx], valid_y[min_idx]),
                    xytext=(10, -20), textcoords='offset points',
                    bbox=dict(boxstyle='round,pad=0.3', fc='yellow', alpha=0.3),
                    arrowprops=dict(arrowstyle='->', connectionstyle='arc3,rad=0'))
        plt.annotate(f'Max: {max(valid_y):.3f}',
                    xy=(valid_x[max_idx], valid_y[max_idx]),
                    xytext=(10, 20), textcoords='offset points',
                    bbox=dict(boxstyle='round,pad=0.3', fc='yellow', alpha=0.3),
                    arrowprops=dict(arrowstyle='->', connectionstyle='arc3,rad=0'))
    
    plt.tight_layout()
    plt.savefig(f'ai_vs_mult_depth.pdf')
    plt.savefig(f'ai_vs_mult_depth.png', dpi=150)
    plt.show()
    
    print(f"\nPlots saved as ai_vs_mult_depth.pdf and .png")
    print(f"Data saved as data_mult_depth_{timestamp}.json")

def plot_ring_dim():
    """Generate plot for ring dimension vs AI."""
    
    ring_dims = [8192, 16384, 32768, 65536]
    mult_depth = 5
    ai_values = []
    
    print("\nBenchmarking ring dimension...")
    print("Ring Dim | AI (ops/byte)")
    print("-" * 30)
    
    for dim in ring_dims:
        ai = run_benchmark_with_args(mult_depth, dim)
        if ai:
            ai_values.append(ai)
            print(f"{dim:8d} | {ai:.6f}")
        else:
            ai_values.append(np.nan)
            print(f"{dim:8d} | FAILED")
    
    # Plot
    plt.figure(figsize=(10, 6))
    valid_x = [d for d, a in zip(ring_dims, ai_values) if not np.isnan(a)]
    valid_y = [a for a in ai_values if not np.isnan(a)]
    
    plt.semilogx(valid_x, valid_y, 'r-s', linewidth=2, markersize=10, basex=2)
    plt.xlabel('Ring Dimension', fontsize=12)
    plt.ylabel('Arithmetic Intensity (ops/byte)', fontsize=12)
    plt.title(f'AI vs Ring Dimension (Mult Depth = {mult_depth})', fontsize=14)
    plt.grid(True, alpha=0.3, which="both")
    plt.xticks(ring_dims, [str(d) for d in ring_dims])
    
    plt.tight_layout()
    plt.savefig('ai_vs_ring_dim.pdf')
    plt.savefig('ai_vs_ring_dim.png', dpi=150)
    plt.show()
    
    print(f"\nPlots saved as ai_vs_ring_dim.pdf and .png")

if __name__ == "__main__":
    import sys
    
    print("=== Arithmetic Intensity Analysis ===\n")
    
    if len(sys.argv) > 1:
        if sys.argv[1] == "ring":
            plot_ring_dim()
        elif sys.argv[1] == "depth":
            plot_mult_depth()
        else:
            print("Usage: python3 plot_ai.py [depth|ring]")
    else:
        # Default: plot multiplicative depth
        plot_mult_depth()