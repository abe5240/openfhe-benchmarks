#!/usr/bin/env python3
import sys
from pathlib import Path

# Add parent directory to path
sys.path.append(str(Path(__file__).parent.parent))

from benchmarker import Benchmarker

b = Benchmarker()
result = b.run("addition", ring_dim=16384, threads=4)

print(f"Latency: {result['latency'][0]:.3f}s")
print(f"DRAM: {result['dram']['DRAM_TOTAL_BYTES'] / 1e9:.2f} GB")
print(f"Ops: {result['opcounts']['total']:,}")
print(f"AI: {result.get('ai', 0):.3f} ops/byte")