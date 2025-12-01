AFL_CC ?= afl-clang-fast
LIBFUZZER_CC ?= clang
CFLAGS ?= -g -fsanitize=address
ARES_FLAGS ?= -Isrc/exec/ezld/include -g3
LIBFUZZER_FLAGS ?= $(ARES_FLAGS) -fsanitize=address -fsanitize=fuzzer
AFL_FLAGS ?= $(ARES_FLAGS) -O2 -fsanitize=address

EXEC_SRC = src/exec/core.c src/exec/emulate.c src/exec/callsan.c src/exec/dev.c
SRC = $(EXEC_SRC) src/exec/vendor/commander.c src/exec/cli.c src/exec/elf.c
AFLSRC = $(EXEC_SRC) src/exec/afl.c
FUZZER_SRC = $(EXEC_SRC) src/exec/libfuzzer.c
TEST_SRC = $(EXEC_SRC) src/test/test.c src/unity/src/unity.c  
LIBEZLD = src/exec/ezld/bin/libezld.a

ares: $(SRC) $(LIBEZLD)
	$(CC) $(CFLAGS) $(ARES_FLAGS) $(SRC) $(LIBEZLD) -o ares

ares_afl: $(AFLSRC) $(LIBEZLD)
	$(AFL_CC) $(CFLAGS) $(AFL_FLAGS) $(AFLSRC) $(LIBEZLD) -o ares_afl

ares_libfuzzer: $(FUZZER_SRC) $(LIBEZLD)
	$(LIBFUZZER_CC) $(CFLAGS) $(LIBFUZZER_FLAGS) $(LIBEZLD) $(FUZZER_SRC) -o ares_libfuzzer

src/test/test_main.c: $(TEST_SRC)
	./src/test/gen_main.sh src/test/test.c > src/test/test_main.c

ares_test: $(TEST_SRC) src/test/test_main.c $(LIBEZLD)
	clang $(CFLAGS) $(ARES_FLAGS) $(TEST_SRC) src/test/test_main.c $(LIBEZLD) -o ares_test -Isrc/unity/src

ares_test_cov: $(TEST_SRC) src/test/test_main.c $(LIBEZLD)
	clang $(CFLAGS) $(ARES_FLAGS) $(TEST_SRC) src/test/test_main.c $(LIBEZLD) -fprofile-instr-generate -fcoverage-mapping -o ares_test -Isrc/unity/src

test_coverage: ares_test_cov
	LLVM_PROFILE_FILE="ares_test.profraw" ./ares_test
	llvm-profdata merge -output=ares_test.profdata ares_test.profraw
	llvm-cov export --format=lcov ./ares_test -instr-profile=ares_test.profdata > lcov.info

$(LIBEZLD):
	cd src/exec/ezld && make library

clean:
	rm -f ares ares_afl ares_libfuzzer
	cd src/exec/ezld && make clean

.PHONY: clean test_coverage
