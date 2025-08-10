from benchmarker import Benchmarker

b = Benchmarker()

b.base_config["ring_dim"] = 32768
b.base_config["matrix_dim"] = 16  
b.base_config["mult_depth"] = 3
b.base_config["num_digits"] = 2
b.base_config["quiet"] = True
# ...

# List of benchmarks to test
benchmarks = [
    "addition",
    "multiplication",
    "rotation",
    "simple-diagonal-method", # ...
]

# Compare all methods
for benchmark in benchmarks:
    print(f"\nRunning {benchmark}...")
    result = b.run(benchmark)

    ai = result['ai']
    #success = result['success']

    # if not success:
    #     print("Fail")
    #     exit()

    print(f"Arithmetic intensity of {benchmark}: {ai:.5f} ops/byte")
