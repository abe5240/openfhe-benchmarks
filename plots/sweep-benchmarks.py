#!/usr/bin/env python3
"""
Test suite for all benchmarks with default parameters.
Verifies correctness and reports arithmetic intensity.
"""

from benchmarker import Benchmarker
import sys
from datetime import datetime

# Configuration
BENCHMARKS = [
    "addition",
    "multiplication",
    "rotation",
    "simple-diagonal-method",
    "single-hoisted-diagonal-method",
    "bsgs-diagonal-method",
    "single-hoisted-bsgs-diagonal-method",
]

def main():
    print("=" * 60)
    print("BENCHMARK TEST SUITE")
    print(f"Started: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}")
    print("=" * 60)
    
    # Create benchmarker with debug off for cleaner output
    b = Benchmarker(debug=False)
    
    # Configure parameters
    b.base_config["ring_dim"]   = 128
    b.base_config["matrix_dim"] = 16
    b.base_config["num_limbs"]  = 4 
    b.base_config["num_digits"] = 2
    
    print("\nConfiguration:")
    print(f"  Ring dimension: {b.base_config['ring_dim']}")
    print(f"  Matrix dimension: {b.base_config['matrix_dim']}")
    print(f"  Number of limbs: {b.base_config['num_limbs']}")
    print(f"  Number of digits: {b.base_config['num_digits']}")
    print()
    
    # Track results
    results = []
    failed = []
    
    # Header for results table
    print(f"{'Benchmark':<40} {'Status':<10} {'AI (ops/byte)':<15}")
    print("-" * 65)
    
    for benchmark in BENCHMARKS:
        # Run benchmark
        result = b.run(benchmark)
        
        # Check success
        if not result['success']:
            print(f"{benchmark:<40} {'FAILED':<10} {'---':<15}")
            failed.append(benchmark)
            continue
        
        # Get AI value
        ai = result['ai']
        ai_str = f"{ai:.5f}" if ai else "N/A"
        
        print(f"{benchmark:<40} {'✓ OK':<10} {ai_str:<15}")
        
        results.append({
            'name': benchmark,
            'ai': ai,
            'latency': result['latency'][0]
        })
    
    # Summary
    print("-" * 65)
    print(f"\nSummary: {len(results)}/{len(BENCHMARKS)} passed")
    
    if failed:
        print(f"\n⚠ Failed benchmarks: {', '.join(failed)}")
        sys.exit(1)
    
    # Print AI ranking
    if results:
        print("\nArithmetic Intensity Ranking (highest to lowest):")
        sorted_results = sorted(
            [r for r in results if r['ai'] is not None], 
            key=lambda x: x['ai'], 
            reverse=True
        )
        for i, r in enumerate(sorted_results, 1):
            print(f"  {i}. {r['name']}: {r['ai']:.5f} ops/byte")
    
    print("\n✓ All benchmarks passed verification")
    return 0

if __name__ == "__main__":
    sys.exit(main())