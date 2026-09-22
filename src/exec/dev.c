#include "ares/dev.h"

#include "ares/core.h"
#include "ares/emulate.h"

#define MMIO_OP_READ 0
#define MMIO_OP_WRITE 1

typedef bool (*DeviceHandler)(AresState *g, u32 devaddr, u8 *buf, u32 op_size,
                              u32 off, int op);

typedef struct {
    DeviceHandler handler;
    u8 buffer[MMIO_DEVICE_RSV];
} Device;

static u32 device_read_u32(u8 *buf, u32 off) {
    u32 value = 0;
    ares_buf_read(buf + off, 4, &value);
    return value;
}

static void device_write_u32(u8 *buf, u32 off, u32 value) {
    ares_buf_write(buf + off, 4, value);
}

static Device g_mmio_devices[];

static void ric_send_interrupt(AresState *g, u32 devaddr) {
    device_write_u32(g_mmio_devices[6].buffer, RIC_REG_DEVADDR, devaddr);
    emulator_interrupt_set_pending(
        g, CAUSE_SUPERVISOR_EXTERNAL & ~CAUSE_INTERRUPT);
}

static bool dma_handler(AresState *g, u32 devaddr, u8 *buf, u32 op_size,
                        u32 off, int op) {
    if (op == MMIO_OP_READ) return true;

    u32 cntl = device_read_u32(buf, DMA_REG_CNTL);
    if (!(cntl & DMA_CNTL_DO)) return true;
    device_write_u32(buf, DMA_REG_CNTL, cntl & ~DMA_CNTL_DO);

    // snapshot the request before DMA writes can alter device registers
    u32 width = device_read_u32(buf, DMA_REG_TRANS_SIZE);
    u32 len = device_read_u32(buf, DMA_REG_LEN);
    if (width != 1 && width != 2 && width != 4) return false;
    if (len % width != 0) return false;
    u32 dst = device_read_u32(buf, DMA_REG_DST_ADDR);
    u32 src = device_read_u32(buf, DMA_REG_SRC_ADDR);
    u32 dst_inc = device_read_u32(buf, DMA_REG_DST_INC);
    u32 src_inc = device_read_u32(buf, DMA_REG_SRC_INC);

    for (u32 remaining = len / width; remaining != 0; remaining--) {
        bool err;
        u32 data = LOAD(g, src, width, &err);
        if (err) return false;
        STORE(g, dst, data, width, &err);
        if (err) return false;
        dst += dst_inc;
        src += src_inc;
    }

    return true;
}

static bool power_handler(AresState *g, u32 devaddr, u8 *buf, u32 op_size,
                          u32 off, int op) {
    if (op == MMIO_OP_READ) return true;

    u32 cntl = device_read_u32(buf, POWER_REG_CNTL);
    if (cntl & POWER_CNTL_SHUTDOWN) emulator_exit(g, 0);

    // TODO: handle restart
    return true;
}

static bool console_handler(AresState *g, u32 devaddr, u8 *buf, u32 op_size,
                            u32 off, int op) {
    u32 out = device_read_u32(buf, CONSOLE_REG_OUT);
    if (op == MMIO_OP_WRITE && off == CONSOLE_REG_OUT) putchar(out);

    // TODO: this should run when input arrives from the user
    u32 cntl = device_read_u32(buf, CONSOLE_REG_CNTL);
    if (cntl & CONSOLE_CNTL_INTERRUPT) {
        u32 in_size = device_read_u32(buf, CONSOLE_REG_IN_SIZE) + 1;
        u32 batch_size = device_read_u32(buf, CONSOLE_REG_BATCH_SIZE);
        if (in_size >= batch_size) {
            in_size -= batch_size;
            ric_send_interrupt(g, devaddr);
        }
        device_write_u32(buf, CONSOLE_REG_IN_SIZE, in_size);
    }

    return true;
}

static bool ric_handler(AresState *g, u32 devaddr, u8 *buf, u32 op_size,
                        u32 off, int op) {
    return op == MMIO_OP_READ;
}

static Device g_mmio_devices[] = {
    [0] = {dma_handler, {0}},      // DMA 0
    [1] = {dma_handler, {0}},      // DMA 1
    [2] = {dma_handler, {0}},      // DMA 2
    [3] = {dma_handler, {0}},      // DMA 3,
    [4] = {power_handler, {0}},    // POWER 0
    [5] = {console_handler, {0}},  // CONSOLE 0
    [6] = {ric_handler, {0}},      // RIC 0
};

bool mmio_read(AresState *g, u32 mmio_addr, int size, u32 *ret) {
    u32 dev_num = mmio_addr / MMIO_DEVICE_RSV;
    u32 dev_addr = MMIO_BASE + dev_num * MMIO_DEVICE_RSV;

    if (dev_num >= sizeof(g_mmio_devices) / sizeof(Device) ||
        (size != 1 && size != 2 && size != 4) ||
        (u32)size > MMIO_DEVICE_RSV - mmio_addr % MMIO_DEVICE_RSV) {
        return false;
    }

    Device *dev = &g_mmio_devices[dev_num];
    u8 *buf = dev->buffer;
    u32 off = mmio_addr - (dev_num * MMIO_DEVICE_RSV);
    bool ok = dev->handler(g, dev_addr, buf, size, off, MMIO_OP_READ);

    if (!ok) {
        *ret = 0;
        return false;
    }

    return ares_buf_read(buf + off, size, ret);
}

bool mmio_write(AresState *g, u32 mmio_addr, int size, u32 value) {
    u32 dev_num = mmio_addr / MMIO_DEVICE_RSV;
    u32 dev_addr = MMIO_BASE + dev_num * MMIO_DEVICE_RSV;

    if (dev_num >= sizeof(g_mmio_devices) / sizeof(Device) ||
        (size != 1 && size != 2 && size != 4) ||
        (u32)size > MMIO_DEVICE_RSV - mmio_addr % MMIO_DEVICE_RSV) {
        return false;
    }

    Device *dev = &g_mmio_devices[dev_num];
    u8 *buf = dev->buffer;
    u32 off = mmio_addr - (dev_num * MMIO_DEVICE_RSV);
    if (!ares_buf_write(buf + off, size, value)) return false;

    return dev->handler(g, dev_addr, buf, size, off, MMIO_OP_WRITE);
}
