// slower test: not run by default as it takes a few minutes
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "../exec/ares/core.h"
#include "../exec/ares/emulate.h"
size_t disassemble(u32 inst, char *buf, size_t buflen);

int main(void) {
    char buf[64];
    for (u64 i = 0x3; i < 1ul << 32; i += 4) {
        size_t buflen = disassemble(i, buf, 64);
        if (buflen > 0 && buf[0] == '<') continue;
        assemble(buf, buflen, false);
        if (g_error != NULL) {
            printf("failure for orig=0x%lx %s error %s\n", i, buf, g_error);
            break;
        }
        bool err = false;
        if (LOAD(g_text->base, 4, &err) != i) {
            printf("failure for orig=0x%lx %s assembled=0x%x\n", i, buf,
                   LOAD(g_text->base, 4, &err));
            break;
        }
        free_runtime();
    }

    for (u64 i = 0; i < 65536; i++) {
        if (i % 4 == 3) continue;
        size_t buflen = disassemble(i, buf, 64);
        if (buflen > 0 && buf[0] == '<') continue;
        assemble(buf, buflen, false);
        if (g_error != NULL) {
            printf("failure for orig=0x%lx %s error %s\n", i, buf, g_error);
            break;
        }
        bool err = false;
        if (LOAD(g_text->base, 2, &err) != i) {
            printf("failure for orig=0x%lx %s assembled=0x%x\n", i, buf,
                   LOAD(g_text->base, 2, &err));
            break;
        }
        free_runtime();
    }
}