#include "ares/dev.h"

#include "ares/emulate.h"

#define MMIO_OP_READ 0
#define MMIO_OP_WRITE 1

typedef bool (*DeviceHandler)(u32 devaddr, u8 *buf, u32 op_size, u32 off,
                              int op);

typedef struct {
    DeviceHandler handler;
    u8 buffer[MMIO_DEVICE_RSV];
} Device;

typedef struct {
    u32 dst_addr;
    u32 src_addr;
    u32 dst_inc;
    u32 src_inc;
    u32 len;
    u32 trans_size;
    u32 cntl;
} PACKED DMAControllerRegisters;

typedef struct {
    char in;
    char out;
    u32 in_size;
    u32 batch_size;
    u32 cntl;
} PACKED ConsoleRegisters;

typedef struct {
    u32 devaddr;
} PACKED RICRegisters;

static Device g_mmio_devices[];

static void ric_send_interrupt(u32 devaddr) {
    RICRegisters *ric = (void *)g_mmio_devices[6].buffer;
    ric->devaddr = devaddr;
    emulator_interrupt_set_pending(CAUSE_SUPERVISOR_EXTERNAL & ~CAUSE_INTERRUPT);
}

static bool dma_handler(u32 devaddr, u8 *buf, u32 op_size, u32 off, int op) {
    if (MMIO_OP_READ == op) {
        return true;
    }

    DMAControllerRegisters *dma = (void *)buf;

    if (!(DMA_CNTL_DO & dma->cntl)) {
        return true;
    }

    dma->cntl &= ~DMA_CNTL_DO;

    for (u32 dst_off = 0, src_off = 0, i = 0; i < dma->len;
         dst_off += dma->dst_inc, src_off += dma->src_inc,
             i += dma->trans_size) {
        u32 dst_addr = dma->dst_addr + dst_off;
        u32 src_addr = dma->src_addr + src_off;

        bool load_err;
        u32 data = LOAD(src_addr, dma->trans_size, &load_err);
        if (load_err) {
            return false;
        }

        bool store_err;
        STORE(dst_addr, data, dma->trans_size, &store_err);
        if (store_err) {
            return false;
        }
    }

    return true;
}

static bool power_handler(u32 devaddr, u8 *buf, u32 op_size, u32 off, int op) {
    if (MMIO_OP_READ == op) {
        return true;
    }

    u8 cntl = *buf;

    if (POWER_CNTL_SHUTDOWN & cntl) {
        emu_exit();
    }

    // TODO: handle restart
    return true;
}

static bool console_handler(u32 devaddr, u8 *buf, u32 op_size, u32 off,
                            int op) {
    ConsoleRegisters *console = (void *)buf;

    if (op == MMIO_OP_READ) {
        if (off == offsetof(ConsoleRegisters, in)) {
            // how?
        }
    } else if (op == MMIO_OP_WRITE) {
        if (off == offsetof(ConsoleRegisters, out)) {
            putchar(console->out);
        }
    }

    // TODO: this should not be here, this shouild be run when reading input
    // from the user
    if (console->cntl & CONSOLE_CNTL_INTERRUPT) {
        console->in_size++;
        if (console->in_size >= console->batch_size) {
            console->in_size = 0;
            ric_send_interrupt(devaddr);
        }
    }

    return true;
}

static bool ric_handler(u32 devaddr, u8 *buf, u32 op_size, u32 off, int op) {
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

bool mmio_read(u32 mmio_addr, int size, u32 *ret) {
    u32 dev_num = mmio_addr / MMIO_DEVICE_RSV;
    u32 dev_addr = MMIO_BASE + dev_num * MMIO_DEVICE_RSV;

    if (dev_num > sizeof(g_mmio_devices) / sizeof(Device)) {
        return false;
    }

    Device *dev = &g_mmio_devices[dev_num];
    u8 *buf = dev->buffer;
    u32 off = mmio_addr - (dev_num * MMIO_DEVICE_RSV);
    bool ok = dev->handler(dev_addr, buf, size, off, MMIO_OP_READ);

    if (!ok) {
        *ret = 0;
        return false;
    }

    return ares_buf_read(buf, size, ret);
}

bool mmio_write(u32 mmio_addr, int size, u32 value) {
    u32 dev_num = mmio_addr / MMIO_DEVICE_RSV;
    u32 dev_addr = MMIO_BASE + dev_num * MMIO_DEVICE_RSV;

    if (dev_num > sizeof(g_mmio_devices) / sizeof(Device)) {
        return false;
    }
    Device *dev = &g_mmio_devices[dev_num];
    u8 *buf = dev->buffer;
    u32 off = mmio_addr - (dev_num * MMIO_DEVICE_RSV);
    if (!ares_buf_write(buf + off, size, value)) {
        return false;
    }

    return dev->handler(dev_addr, buf, size, off, MMIO_OP_WRITE);
}
