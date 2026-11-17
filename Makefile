# SPDX-License-Identifier: Apache-2.0
CXX      ?= clang++
CXXFLAGS ?= -O3 -std=c++17 -Wall -Wextra -Werror
BUILD    ?= build

LIB_SRCS := src/nev_container.cpp src/arw_container.cpp src/ticoraw_dec.cpp src/arw6_dec.cpp src/dng_writer.cpp
LIB_OBJS := $(BUILD)/nev_container.o $(BUILD)/arw_container.o $(BUILD)/ticoraw_dec.o $(BUILD)/arw6_dec.o $(BUILD)/dng_writer.o
HDRS     := include/nijinWVDC.h src/tiff_internal.h

TARGET      := $(BUILD)/nijinWVDC
TEST_TARGET := $(BUILD)/test_nijinWVDC
ASAN_CLI    := $(BUILD)/nijinWVDC_asan
ASAN_TEST   := $(BUILD)/test_nijinWVDC_asan

.PHONY: all clean asan test

all: $(TARGET)

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/%.o: src/%.cpp $(HDRS) | $(BUILD)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(TARGET): src/main.cpp $(LIB_OBJS) $(HDRS) | $(BUILD)
	$(CXX) $(CXXFLAGS) src/main.cpp $(LIB_OBJS) -lpthread -o $@

$(TEST_TARGET): tests/test_nijinWVDC.cpp $(LIB_OBJS) $(HDRS) | $(BUILD)
	$(CXX) $(CXXFLAGS) tests/test_nijinWVDC.cpp $(LIB_OBJS) -lpthread -o $@

asan: | $(BUILD)
	$(CXX) -O1 -g -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
		src/main.cpp $(LIB_SRCS) -lpthread -o $(ASAN_CLI)
	$(CXX) -O1 -g -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
		tests/test_nijinWVDC.cpp $(LIB_SRCS) -lpthread -o $(ASAN_TEST)

test: $(TEST_TARGET) asan
	./$(TEST_TARGET)
	./$(ASAN_TEST)

clean:
	rm -rf $(BUILD)
