AFL_CC ?= afl-clang-lto
LIBFUZZER_CC ?= clang
CFLAGS ?= -g
ARES_FLAGS ?= -g3
LIBFUZZER_FLAGS ?= $(ARES_FLAGS) -fsanitize=address -fsanitize=fuzzer
AFL_FLAGS ?= $(ARES_FLAGS) -O2 -fsanitize=address,undefined

EXEC_SRC = src/exec/core.c src/exec/emulate.c src/exec/callsan.c src/exec/dev.c src/exec/elf.c
SRC = $(EXEC_SRC) src/exec/cli.c
AFLSRC = $(EXEC_SRC) src/exec/afl.c
FUZZER_SRC = $(EXEC_SRC) src/exec/libfuzzer.c
TEST_SRC = $(EXEC_SRC) src/test/test.c src/unity/src/unity.c  

BIN_DIR = bin
TARGETS = $(BIN_DIR)/ares $(BIN_DIR)/ares_afl $(BIN_DIR)/ares_libfuzzer_asm \
		  $(BIN_DIR)/ares_libfuzzer_elf $(BIN_DIR)/ares_test

all: $(BIN_DIR)/ares

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

$(BIN_DIR)/ares: $(SRC) | $(BIN_DIR)
	$(CC) $(CFLAGS) $(ARES_FLAGS) $(SRC) -o $@

$(BIN_DIR)/ares_afl: $(AFLSRC) | $(BIN_DIR)
	$(AFL_CC) $(CFLAGS) $(AFL_FLAGS) $(AFLSRC) -o $@

$(BIN_DIR)/ares_libfuzzer_asm: $(FUZZER_SRC) | $(BIN_DIR)
	$(LIBFUZZER_CC) $(CFLAGS) $(LIBFUZZER_FLAGS) -DFUZZ_ASM  $(FUZZER_SRC) -o $@

$(BIN_DIR)/ares_libfuzzer_elf: $(FUZZER_SRC) | $(BIN_DIR)
	$(LIBFUZZER_CC) $(CFLAGS) $(LIBFUZZER_FLAGS) -DFUZZ_ELF  $(FUZZER_SRC) -o $@

src/test/test_main.c: $(TEST_SRC)
	./src/test/gen_main.sh src/test/test.c > src/test/test_main.c

$(BIN_DIR)/ares_test: $(TEST_SRC) src/test/test_main.c | $(BIN_DIR)
	$(CC) $(CFLAGS) $(ARES_FLAGS) $(TEST_SRC) src/test/test_main.c -o $@ -Isrc/unity/src

$(BIN_DIR)/ares_test_roundtrip: $(TEST_SRC) src/test/test_roundtrip.c | $(BIN_DIR)
	$(CC) $(CFLAGS) $(ARES_FLAGS) $(TEST_SRC) src/test/test_roundtrip.c -o $@ -Isrc/unity/src -O3 -flto -g3 -fno-omit-frame-pointer -g3

$(BIN_DIR)/ares_test_cov: $(TEST_SRC) src/test/test_main.c | $(BIN_DIR)
	clang $(CFLAGS) $(ARES_FLAGS) $(TEST_SRC) src/test/test_main.c -fprofile-instr-generate -fcoverage-mapping -o $@ -Isrc/unity/src

test_coverage: $(BIN_DIR)/ares_test_cov
	LLVM_PROFILE_FILE="./$(BIN_DIR)/ares_test_cov.profraw" ./$(BIN_DIR)/ares_test_cov
	llvm-profdata merge -output=./$(BIN_DIR)/ares_test_cov.profdata ./$(BIN_DIR)/ares_test_cov.profraw
	llvm-cov export --format=lcov ./$(BIN_DIR)/ares_test_cov -instr-profile=./$(BIN_DIR)/ares_test_cov.profdata > lcov.info

clean:
	rm -f $(BIN_DIR)/ares $(BIN_DIR)/ares_afl $(BIN_DIR)/ares_libfuzzer $(BIN_DIR)/ares_test $(BIN_DIR)/ares_test_cov

.PHONY: clean test_coverage all
