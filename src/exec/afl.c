#include <unistd.h>
#include "ares/core.h"

__AFL_FUZZ_INIT();

int main(void)
{
    __AFL_INIT();
    char *src = 0;
    unsigned char *buf = __AFL_FUZZ_TESTCASE_BUF;
    while (__AFL_LOOP(10000)) {
        int len = __AFL_FUZZ_TESTCASE_LEN;
        src = realloc(src, len);
        memcpy(src, buf, len);
        assemble(src, len, false);
    }
}
