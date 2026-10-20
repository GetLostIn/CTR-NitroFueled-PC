/* SPDX-License-Identifier: GPL-2.0-or-later */
/* No guest image exists in these host tests. Main's optional online diagnostics
 * may query its presence, but must never read or patch guest memory here. */
#include "runtime.h"
#include <stdlib.h>
__thread sigjmp_buf *runtime_fault_recover;
uintptr_t runtime_probe_image(uint64_t *size) {
    if (size) *size = 0;
    return 0;
}
void runtime_probe_read_image(uintptr_t address, void *data, uint64_t size) {
    (void)address; (void)data; (void)size;
    abort();
}
int runtime_probe_write_image(uintptr_t address, const void *data, uint64_t size) {
    (void)address; (void)data; (void)size;
    abort();
}
void runtime_memory_read_backing(uintptr_t offset, void *out, uint64_t size) {
    (void)offset; (void)out; (void)size;
    abort();
}
