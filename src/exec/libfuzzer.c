#include "ares/core.h"
#include "ares/elf.h"

int LLVMFuzzerTestOneInput(const uint8_t *Data, size_t Size) {
    // Test assembler
    assemble((const char *)Data, Size, false);
    free_runtime();

    // Test readelf
    char *e;
    ReadElfResult r = {0};
    elf_read((u8 *)Data, Size, &r, &e);
    free(r.phdrs);
    free(r.shdrs);

    return 0;
}
