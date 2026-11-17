# SPDX-License-Identifier: Apache-2.0
CXX      ?= clang++
CC       ?= cc
CXXFLAGS ?= -O3 -std=c++17 -Wall -Wextra -Werror
CFLAGS   ?= -O2 -std=c11 -Wall -Wextra -Werror
BUILD    ?= build

UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
    SHLIB_EXT   := dylib
    SHLIB_FLAGS := -dynamiclib -Wl,-install_name,@rpath/libnijinWVDC.dylib
    RPATH_FLAG  := -Wl,-rpath,@executable_path
else
    SHLIB_EXT   := so
    SHLIB_FLAGS := -shared -Wl,-soname,libnijinWVDC.so.0
    RPATH_FLAG  := -Wl,-rpath,'$$ORIGIN'
endif

LIB_SRCS := src/nev_container.cpp src/arw_container.cpp src/ticoraw_dec.cpp \
            src/arw6_dec.cpp src/dng_writer.cpp src/session_api.cpp src/c_api.cpp
STATIC_OBJS := $(patsubst src/%.cpp,$(BUILD)/static/%.o,$(LIB_SRCS))
SHARED_OBJS := $(patsubst src/%.cpp,$(BUILD)/shared/%.o,$(LIB_SRCS))
HDRS := include/nijinWVDC.h include/nijinWVDC_c.h include/nijinWVDC_export.h \
        src/tiff_internal.h src/worker_pool.h src/simd_platform.h src/codec_internal.h

STATIC_LIB   := $(BUILD)/libnijinWVDC.a
SHARED_LIB   := $(BUILD)/libnijinWVDC.$(SHLIB_EXT)
TARGET       := $(BUILD)/nijinWVDC
TEST_TARGET  := $(BUILD)/test_nijinWVDC
TEST_C       := $(BUILD)/test_c_api
ASAN_CLI     := $(BUILD)/nijinWVDC_asan
ASAN_TEST    := $(BUILD)/test_nijinWVDC_asan
ASAN_C_TEST  := $(BUILD)/test_c_api_asan

.PHONY: all shared clean asan test

all: $(TARGET) $(STATIC_LIB) $(SHARED_LIB)

shared: $(SHARED_LIB)

$(BUILD) $(BUILD)/static $(BUILD)/shared:
	mkdir -p $@

$(BUILD)/static/%.o: src/%.cpp $(HDRS) | $(BUILD)/static
	$(CXX) $(CXXFLAGS) -DNIJINWVDC_STATIC -c $< -o $@

$(BUILD)/shared/%.o: src/%.cpp $(HDRS) | $(BUILD)/shared
	$(CXX) $(CXXFLAGS) -fPIC -fvisibility=hidden -DNIJINWVDC_BUILDING_DLL -c $< -o $@

$(STATIC_LIB): $(STATIC_OBJS)
	ar rcs $@ $(STATIC_OBJS)

$(SHARED_LIB): $(SHARED_OBJS)
	$(CXX) $(SHLIB_FLAGS) $(SHARED_OBJS) -lpthread -lm -o $@

$(TARGET): src/main.cpp $(STATIC_LIB) $(HDRS) | $(BUILD)
	$(CXX) $(CXXFLAGS) -DNIJINWVDC_STATIC src/main.cpp $(STATIC_LIB) -lpthread -lm -o $@

$(TEST_TARGET): tests/test_nijinWVDC.cpp $(STATIC_LIB) $(HDRS) | $(BUILD)
	$(CXX) $(CXXFLAGS) -DNIJINWVDC_STATIC tests/test_nijinWVDC.cpp $(STATIC_LIB) -lpthread -lm -o $@

$(TEST_C): tests/test_c_api.c $(SHARED_LIB) include/nijinWVDC_c.h include/nijinWVDC_export.h | $(BUILD)
	$(CC) $(CFLAGS) tests/test_c_api.c $(SHARED_LIB) $(RPATH_FLAG) -lm -o $@

asan: | $(BUILD)
	$(CXX) -O1 -g -std=c++17 -Wall -Wextra -Werror -DNIJINWVDC_STATIC \
		-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
		src/main.cpp $(LIB_SRCS) -lpthread -lm -o $(ASAN_CLI)
	$(CXX) -O1 -g -std=c++17 -Wall -Wextra -Werror -DNIJINWVDC_STATIC \
		-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
		tests/test_nijinWVDC.cpp $(LIB_SRCS) -lpthread -lm -o $(ASAN_TEST)
	$(CC) -O1 -g -std=c11 -Wall -Wextra -Werror -DNIJINWVDC_STATIC \
		-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
		-c tests/test_c_api.c -o $(BUILD)/test_c_api_asan.o
	$(CXX) -O1 -g -std=c++17 -DNIJINWVDC_STATIC \
		-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
		$(BUILD)/test_c_api_asan.o $(LIB_SRCS) -lpthread -lm -o $(ASAN_C_TEST)

test: $(TEST_TARGET) $(TEST_C) asan
	./$(TEST_TARGET)
	./$(TEST_C)
	./$(ASAN_TEST)
	./$(ASAN_C_TEST)

clean:
	rm -rf $(BUILD)
