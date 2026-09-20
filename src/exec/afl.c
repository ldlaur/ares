#include <unistd.h>

#include "ares/emulate.h"

__AFL_FUZZ_INIT();

int main(void) {
    __AFL_INIT();
    AresState *g = calloc(1, sizeof(*g));
    ares_panic_if_null(g);
    unsigned char *buf = __AFL_FUZZ_TESTCASE_BUF;
    while (__AFL_LOOP(10000)) {
        size_t len = __AFL_FUZZ_TESTCASE_LEN;
        assemble(g, (const char *)buf, len, false);
        free_runtime(g);
    }
    free(g);
}
