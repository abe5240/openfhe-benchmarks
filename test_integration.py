#!/usr/bin/env python3
"""
test_integration.py - Test that PIN tool and DRAM counter integration works
"""

import subprocess
from pathlib import Path


def test_validation_program():
    """First, test with the validation program to ensure PIN is working"""
    print("=" * 60)
    print("Testing PIN tool with validation program")
    print("=" * 60)
    
    # Build validation program if needed
    validation_cpp = """
// validation.cpp – Test PIN tool integration
#include "dram_counter.hpp"
#include <cstdint>
#include <cstdio>

extern "C" {
    void __attribute__((noinline)) PIN_MARKER_START() { asm volatile(""); }
    void __attribute__((noinline)) PIN_MARKER_END()   { asm volatile(""); }
}

static DRAMCounter g_dram;

static void ArithmeticKernel() {
    constexpr int ITERS = 1'000;
    uint64_t a = 1, b = 2, c = 3, d = 5;
    
    for (int i = 0; i < ITERS; ++i) {
        asm volatile(
            "addq  %[b], %[a]\\n\\t"
            "subq  %[d], %[c]\\n\\t"
            "imulq %[a], %[b]\\n\\t"
            "xorq  %%rdx, %%rdx\\n\\t"
            "movq  %[a], %%rax\\n\\t"
            "divq  %[c]\\n\\t"
            : [a] "+r"(a), [b] "+r"(b), [c] "+r"(c)
            : [d] "r"(d)
            : "rax", "rdx", "cc");
    }
}

int main() {
    std::puts("=== PIN Tool Validation ===");
    
    if (!g_dram.init()) {
        std::puts("Warning: DRAM counters not initialized");
    }
    
    PIN_MARKER_START();
    g_dram.start();
    
    ArithmeticKernel();  // Should count ~4000 ops
    
    g_dram.stop();
    PIN_MARKER_END();
    
    g_dram.print_results(true);  // Save to logs/dram_counts.out
    
    return 0;
}
"""
    
    # Write validation.cpp
    with open("validation.cpp", "w") as f:
        f.write(validation_cpp)
    
    print("Building validation program...")
    result = subprocess.run(
        ["g++", "-O3", "-o", "validation", "validation.cpp", 
         "-I.", "-std=c++17"],
        capture_output=True,
        text=True
    )
    
    if result.returncode != 0:
        print(f"Build failed: {result.stderr}")
        return False
    
    # Run with PIN
    pin_path = "/opt/intel/pin/pin"
    pintool_path = "/opt/profiling-tools/lib/pintool.so"
    
    if not Path(pin_path).exists():
        print(f"PIN not found at {pin_path}")
        return False
    
    print("\nRunning validation with PIN...")
    result = subprocess.run(
        ["sudo", pin_path, "-t", pintool_path, "-quiet", "--", "./validation"],
        capture_output=True,
        text=True
    )
    
    print(result.stdout)
    
    # Check PIN output
    try:
        with open("logs/int_counts.out", "r") as f:
            content = f.read()
            print(f"\nPIN output (logs/int_counts.out):\n{content}")
            
            # Parse the count
            if "TOTAL counted:" in content:
                count = int(content.split("TOTAL counted:")[1].strip())
                print(f"✓ PIN counted {count} operations (expected ~4000)")
                if 3000 < count < 5000:
                    print("✓ PIN tool is working correctly!")
                    return True
                else:
                    print("✗ Unexpected operation count")
    except Exception as e:
        print(f"✗ Could not read PIN output: {e}")
    
    return False


def test_benchmark_integration():
    """Test the actual benchmark with PIN"""
    print("\n" + "=" * 60)
    print("Testing addition benchmark integration")
    print("=" * 60)
    
    from bench_runner import BenchmarkRunner
    
    # Test with PIN
    print("\n1. Testing WITH PIN instrumentation:")
    runner_with_pin = BenchmarkRunner(".", use_pin=True)
    result = runner_with_pin.run(
        "addition",
        params={"ring_dim": 8192}  # Small for quick test
    )
    
    print(f"\nResults with PIN:")
    print(f"  Has PIN data: {result.has_pin_data}")
    print(f"  Has DRAM data: {result.has_dram_data}")
    if result.has_pin_data:
        print(f"  Integer ops: {result.integer_ops:,}")
    if result.has_dram_data:
        print(f"  DRAM bytes: {result.dram_total_bytes:,}")
    if result.arithmetic_intensity:
        print(f"  Arithmetic intensity: {result.arithmetic_intensity:.6f} ops/byte")
    
    # Test without PIN
    print("\n2. Testing WITHOUT PIN instrumentation:")
    runner_no_pin = BenchmarkRunner(".", use_pin=False)
    result = runner_no_pin.run(
        "addition",
        params={"ring_dim": 8192}
    )
    
    print(f"\nResults without PIN:")
    print(f"  Has PIN data: {result.has_pin_data}")
    print(f"  Has DRAM data: {result.has_dram_data}")
    if result.has_dram_data:
        print(f"  DRAM bytes: {result.dram_total_bytes:,}")
    
    return True


def check_prerequisites():
    """Check if all required components are present"""
    print("Checking prerequisites...")
    
    checks = {
        "PIN binary": Path("/opt/intel/pin/pin"),
        "PIN tool": Path("/opt/profiling-tools/lib/pintool.so"),
        "utils.hpp": Path("examples/utils.hpp"),
        "dram_counter.hpp": Path("dram_counter.hpp"),
        "addition.cpp": Path("examples/addition.cpp")
    }
    
    all_present = True
    for name, path in checks.items():
        if path.exists():
            print(f"  ✓ {name}: {path}")
        else:
            print(f"  ✗ {name}: NOT FOUND at {path}")
            all_present = False
    
    return all_present


if __name__ == "__main__":
    print("PIN Tool and DRAM Counter Integration Test")
    print("=" * 60)
    
    if not check_prerequisites():
        print("\n✗ Missing prerequisites. Please install PIN and ensure files are in place.")
        exit(1)
    
    # Create logs directory
    Path("logs").mkdir(exist_ok=True)
    
    # Test validation program
    if test_validation_program():
        print("\n✓ PIN tool validation passed!")
        
        # Test benchmark integration
        try:
            if test_benchmark_integration():
                print("\n✓ All integration tests passed!")
        except ImportError:
            print("\n✗ Could not import bench_runner.py")
            print("  Make sure bench_runner.py is in the current directory")
    else:
        print("\n✗ PIN tool validation failed")
        print("  Make sure PIN is installed and you're running with sudo")