from benchmarker import Benchmarker

b = Benchmarker()

# Thread sweep
thread_counts = [1, 2, 4, 8, 16, 32]
results = []

for threads in thread_counts:
    print(f"Running with {threads} threads...")
    result = b.run("addition", ring_dim=16384, num_digits=2, threads=threads)
    
    latency = result['latency'][0] if result['latency'] else None
    results.append((threads, latency))
    
    if latency:
        print(f"  {threads} threads: {latency:.3f}s")

# Print summary
print("\nThread Scaling Summary:")
print("Threads | Latency (s)")
print("--------|------------")
for threads, latency in results:
    if latency:
        print(f"{threads:7d} | {latency:.3f}")
        
# Calculate speedup
if results[0][1]:  # If single-thread result exists
    baseline = results[0][1]
    print("\nSpeedup relative to 1 thread:")
    for threads, latency in results:
        if latency:
            speedup = baseline / latency
            print(f"{threads:2d} threads: {speedup:.2f}x")