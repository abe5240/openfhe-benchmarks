# paths
BENCH_DIR := examples
BUILD_DIR := build

# generic rule generator
# $1 = target name (what you type after 'make')
# $2 = source file
# $3 = executable name (filename without .cpp)
define bench
$(1):
	cmake -S . -B $(BUILD_DIR) -DBENCH_SOURCE=$(BENCH_DIR)/$(2)
	cmake --build $(BUILD_DIR)
	./profile.sh $(BUILD_DIR)/$(3)
endef

# targets (build + profile)
$(eval $(call bench,addition,addition.cpp,addition))
$(eval $(call bench,multiplication,multiplication.cpp,multiplication))
$(eval $(call bench,rotation,rotation.cpp,rotation))
$(eval $(call bench,simple-diagonal-method,simple-diagonal-method.cpp,simple-diagonal-method))
$(eval $(call bench,single-hoisted-diagonal-method,single-hoisted-diagonal-method.cpp,single-hoisted-diagonal-method))
$(eval $(call bench,bsgs-diagonal-method,bsgs-diagonal-method.cpp,bsgs-diagonal-method))
$(eval $(call bench,single-hoisted-bsgs-diagonal-method,single-hoisted-bsgs-diagonal-method.cpp,single-hoisted-bsgs-diagonal-method))

# housekeeping
.PHONY: addition \
        multiplication \
        rotation \
        simple-diagonal-method \
        single-hoisted-diagonal-method \
        bsgs-diagonal-method \
        single-hoisted-bsgs-diagonal-method \
        clean

clean:
	rm -rf $(BUILD_DIR) logs data