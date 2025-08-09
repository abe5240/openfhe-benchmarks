# OpenFHE Benchmarks Makefile
BUILD_DIR := build
BENCH_DIR := examples

# Build rule for benchmarks
define build_bench
$(1):
	@cmake -S . -B $(BUILD_DIR) -DBENCH_SOURCE=$(BENCH_DIR)/$(1).cpp -DCMAKE_BUILD_TYPE=Release
	@cmake --build $(BUILD_DIR) -j$(nproc)
endef

# Benchmarks
BENCHMARKS := addition multiplication rotation \
              simple-diagonal-method single-hoisted-diagonal-method \
              bsgs-diagonal-method single-hoisted-bsgs-diagonal-method

# Generate rules
$(foreach bench,$(BENCHMARKS),$(eval $(call build_bench,$(bench))))

clean:
	rm -rf $(BUILD_DIR)

.PHONY: $(BENCHMARKS) clean