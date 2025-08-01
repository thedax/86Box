/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          808x CPU emulation, mostly ported from reenigne's XTCE, which
 *          is cycle-accurate.
 *
 * Authors: Andrew Jenner, <https://www.reenigne.org>
 *          Miran Grca, <mgrca8@gmail.com>
 *
 *          Copyright 2015-2020 Andrew Jenner.
 *          Copyright 2016-2020 Miran Grca.
 */
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include "i8080.h"

#define HAVE_STDARG_H
#include <86box/86box.h>
#include "cpu.h"
#include "x86.h"
#include <86box/machine.h>
#include <86box/io.h>
#include <86box/mem.h>
#include <86box/rom.h>
#include <86box/nmi.h>
#include <86box/pic.h>
#include <86box/ppi.h>
#include <86box/timer.h>
#include <86box/gdbstub.h>
#include <86box/plat_fallthrough.h>
#include <86box/plat_unused.h>

/* Is the CPU 8088 or 8086. */
int is8086 = 0;

uint8_t  use_custom_nmi_vector = 0;
uint32_t custom_nmi_vector     = 0x00000000;

/* The prefetch queue (4 bytes for 8088, 6 bytes for 8086). */
static uint8_t pfq[6];

/* Variables to aid with the prefetch queue operation. */
static int biu_cycles = 0, pfq_pos = 0;

/* The IP equivalent of the current prefetch queue position. */
static uint16_t pfq_ip;

/* Pointer tables needed for segment overrides. */
static uint32_t *opseg[4];
static x86seg   *_opseg[4];

static int noint   = 0;
static int cpu_alu_op, pfq_size;

static uint32_t cpu_src = 0, cpu_dest = 0;
static uint32_t cpu_data = 0;

static uint16_t last_addr = 0x0000;

static uint32_t *ovr_seg     = NULL;
static int       prefetching = 1, completed = 1;
static int       in_rep = 0, repeating = 0, rep_c_flag = 0;
static int       oldc, clear_lock = 0;
static int       refresh = 0, cycdiff;

static i8080 emulated_processor;
static bool cpu_md_write_disable = 1;

/* Various things needed for 8087. */
#define OP_TABLE(name) ops_##name

#define CPU_BLOCK_END()
#define SEG_CHECK_READ(seg)
#define SEG_CHECK_WRITE(seg)
#define CHECK_READ(a, b, c)
#define CHECK_WRITE(a, b, c)
#define UN_USED(x) (void) (x)
#define fetch_ea_16(val)
#define fetch_ea_32(val)
#define PREFETCH_RUN(a, b, c, d, e, f, g, h)

#define CYCLES(val)        \
    {                      \
        wait_cycs(val, 0); \
    }

#define CLOCK_CYCLES_ALWAYS(val) \
    {                            \
        wait_cycs(val, 0);       \
    }

#if 0
#    define CLOCK_CYCLES_FPU(val) \
        {                         \
            wait_cycs(val, 0);    \
        }

#    define CLOCK_CYCLES(val)          \
        {                              \
            if (fpu_cycles > 0) {      \
                fpu_cycles -= (val);   \
                if (fpu_cycles < 0) {  \
                    wait_cycs(val, 0); \
                }                      \
            } else {                   \
                wait_cycs(val, 0);     \
            }                          \
        }

#    define CONCURRENCY_CYCLES(c) fpu_cycles = (c)
#else
#    define CLOCK_CYCLES(val)  \
        {                      \
            wait_cycs(val, 0); \
        }

#    define CLOCK_CYCLES_FPU(val) \
        {                         \
            wait_cycs(val, 0);    \
        }

#    define CONCURRENCY_CYCLES(c)
#endif

typedef int (*OpFn)(uint32_t fetchdat);

static int tempc_fpu = 0;

#ifdef ENABLE_808X_LOG
#if 0
void dumpregs(int);
#endif
int x808x_do_log = ENABLE_808X_LOG;

static void
x808x_log(const char *fmt, ...)
{
    va_list ap;

    if (x808x_do_log) {
        va_start(ap, fmt);
        pclog_ex(fmt, ap);
        va_end(ap);
    }
}
#else
#    define x808x_log(fmt, ...)
#endif

static void pfq_add(int c, int add);
static void set_pzs(int bits);

void
prefetch_queue_set_pos(int pos)
{
    pfq_pos = pos;
}

void
prefetch_queue_set_ip(uint16_t ip)
{
    pfq_ip = ip;
}

void
prefetch_queue_set_prefetching(int p)
{
    prefetching = p;
}

int
prefetch_queue_get_pos(void)
{
    return pfq_pos;
}

uint16_t
prefetch_queue_get_ip(void)
{
    return pfq_ip;
}

int
prefetch_queue_get_prefetching(void)
{
    return prefetching;
}

int
prefetch_queue_get_size(void)
{
    return pfq_size;
}
static void set_if(int cond);

void
sync_from_i8080(void)
{
    AL = emulated_processor.a;
    BH = emulated_processor.h;
    BL = emulated_processor.l;
    CH = emulated_processor.b;
    CL = emulated_processor.c;
    DH = emulated_processor.d;
    DL = emulated_processor.e;
    BP = emulated_processor.sp;
    
    cpu_state.pc = emulated_processor.pc;
    cpu_state.flags &= 0xFF00;
    cpu_state.flags |= emulated_processor.sf << 7;
    cpu_state.flags |= emulated_processor.zf << 6;
    cpu_state.flags |= emulated_processor.hf << 4;
    cpu_state.flags |= emulated_processor.pf << 2;
    cpu_state.flags |= 1 << 1;
    cpu_state.flags |= emulated_processor.cf << 0;
    set_if(emulated_processor.iff);
}

void
sync_to_i8080(void)
{
    if (!is_nec)
        return;
    emulated_processor.a  = AL;
    emulated_processor.h  = BH;
    emulated_processor.l  = BL;
    emulated_processor.b  = CH;
    emulated_processor.c  = CL;
    emulated_processor.d  = DH;
    emulated_processor.e  = DL;
    emulated_processor.sp = BP;
    emulated_processor.pc = cpu_state.pc;
    emulated_processor.iff = !!(cpu_state.flags & I_FLAG);

    emulated_processor.sf = (cpu_state.flags >> 7) & 1;
    emulated_processor.zf = (cpu_state.flags >> 6) & 1;
    emulated_processor.hf = (cpu_state.flags >> 4) & 1;
    emulated_processor.pf = (cpu_state.flags >> 2) & 1;
    emulated_processor.cf = (cpu_state.flags >> 0) & 1;

    emulated_processor.interrupt_delay = noint;
}


uint16_t
get_last_addr(void)
{
    return last_addr;
}

static void
clock_start(void)
{
    cycdiff = cycles;
}

static void
clock_end(void)
{
    int diff = cycdiff - cycles;

    /* On 808x systems, clock speed is usually crystal frequency divided by an integer. */
    tsc += (uint64_t) diff * ((uint64_t) xt_cpu_multi >> 32ULL); /* Shift xt_cpu_multi by 32 bits to the right and then multiply. */
    if (TIMER_VAL_LESS_THAN_VAL(timer_target, (uint32_t) tsc))
        timer_process();
}

static void
fetch_and_bus(int c, int bus)
{
    if (refresh > 0) {
        /* Finish the current fetch, if any. */
        cycles -= ((4 - (biu_cycles & 3)) & 3);
        pfq_add((4 - (biu_cycles & 3)) & 3, 1);
        /* Add 4 memory access cycles. */
        cycles -= 4;
        pfq_add(4, 0);

        refresh--;
    }

    pfq_add(c, !bus);
    if (bus < 2) {
        clock_end();
        clock_start();
    }
}

static void
wait_cycs(int c, int bus)
{
    cycles -= c;
    fetch_and_bus(c, bus);
}

/* This is for external subtraction of cycles. */
void
sub_cycles(int c)
{
    if (c <= 0)
        return;

    cycles -= c;

    if (!is286)
        fetch_and_bus(c, 2);
}

void
resub_cycles(int old_cycles)
{
    int cyc_diff = 0;

    if (old_cycles > cycles) {
        cyc_diff = old_cycles - cycles;
        cycles   = old_cycles;
        sub_cycles(cyc_diff);
    }
}

#undef readmemb
#undef readmemw
#undef readmeml
#undef readmemq

static void
cpu_io(int bits, int out, uint16_t port)
{
    int old_cycles = cycles;

    if (out) {
        wait_cycs(is_mazovia ? 5 : 4, 1);
        if (bits == 16) {
            if (is8086 && !(port & 1)) {
                old_cycles = cycles;
                outw(port, AX);
            } else {
                wait_cycs(is_mazovia ? 5 : 4, 1);
                old_cycles = cycles;
                outb(port++, AL);
                outb(port, AH);
            }
        } else {
            old_cycles = cycles;
            outb(port, AL);
        }
    } else {
        wait_cycs(is_mazovia ? 5 : 4, 1);
        if (bits == 16) {
            if (is8086 && !(port & 1)) {
                old_cycles = cycles;
                AX         = inw(port);
            } else {
                wait_cycs(is_mazovia ? 5 : 4, 1);
                old_cycles = cycles;
                AL         = inb(port++);
                AH         = inb(port);
            }
        } else {
            old_cycles = cycles;
            AL         = inb(port);
        }
    }

    resub_cycles(old_cycles);
}

/* Reads a byte from the memory and advances the BIU. */
static uint8_t
readmemb(uint32_t a)
{
    uint8_t ret;

    wait_cycs(4, 1);
    ret = read_mem_b(a);

    return ret;
}

/* Reads a byte from the memory but does not advance the BIU. */
static uint8_t
readmembf(uint32_t a)
{
    uint8_t ret;

    a   = cs + (a & 0xffff);
    ret = read_mem_b(a);

    return ret;
}

/* Reads a word from the memory and advances the BIU. */
static uint16_t
readmemw(uint32_t s, uint16_t a)
{
    uint16_t ret;

    wait_cycs(4, 1);
    if (is8086 && !(a & 1))
        ret = read_mem_w(s + a);
    else {
        wait_cycs(4, 1);
        ret = read_mem_b(s + a);
        ret |= read_mem_b(s + ((is186 && !is_nec) ? (a + 1) : (a + 1) & 0xffff)) << 8;
    }

    return ret;
}

static uint16_t
readmemwf(uint16_t a)
{
    uint16_t ret;

    ret = read_mem_w(cs + (a & 0xffff));

    return ret;
}

static uint16_t
readmem(uint32_t s)
{
    if (opcode & 1)
        return readmemw(s, cpu_state.eaaddr);
    else
        return (uint16_t) readmemb(s + cpu_state.eaaddr);
}

static uint32_t
readmeml(uint32_t s, uint16_t a)
{
    uint32_t temp;

    temp = (uint32_t) (readmemw(s, a + 2)) << 16;
    temp |= readmemw(s, a);

    return temp;
}

static uint64_t
readmemq(uint32_t s, uint16_t a)
{
    uint64_t temp;

    temp = (uint64_t) (readmeml(s, a + 4)) << 32;
    temp |= readmeml(s, a);

    return temp;
}

/* Writes a byte to the memory and advances the BIU. */
static void
writememb(uint32_t s, uint32_t a, uint8_t v)
{
    uint32_t addr = s + a;

    wait_cycs(4, 1);
    write_mem_b(addr, v);

    if ((addr >= 0xf0000) && (addr <= 0xfffff))
        last_addr = addr & 0xffff;
}

/* Writes a word to the memory and advances the BIU. */
static void
writememw(uint32_t s, uint32_t a, uint16_t v)
{
    uint32_t addr = s + a;

    wait_cycs(4, 1);
    if (is8086 && !(a & 1))
        write_mem_w(addr, v);
    else {
        write_mem_b(addr, v & 0xff);
        wait_cycs(4, 1);
        addr = s + ((is186 && !is_nec) ? (a + 1) : ((a + 1) & 0xffff));
        write_mem_b(addr, v >> 8);
    }

    if ((addr >= 0xf0000) && (addr <= 0xfffff))
        last_addr = addr & 0xffff;
}

static void
writemem(uint32_t s, uint16_t v)
{
    if (opcode & 1)
        writememw(s, cpu_state.eaaddr, v);
    else
        writememb(s, cpu_state.eaaddr, (uint8_t) (v & 0xff));
}

static void
writememl(uint32_t s, uint32_t a, uint32_t v)
{
    writememw(s, a, v & 0xffff);
    writememw(s, a + 2, v >> 16);
}

static void
writememq(uint32_t s, uint32_t a, uint64_t v)
{
    writememl(s, a, v & 0xffffffff);
    writememl(s, a + 4, v >> 32);
}

static void
pfq_write(void)
{
    uint16_t tempw;

    if (is8086 && (pfq_pos < (pfq_size - 1))) {
        /* The 8086 fetches 2 bytes at a time, and only if there's at least 2 bytes
           free in the queue. */
        tempw                         = readmemwf(pfq_ip);
        *(uint16_t *) &(pfq[pfq_pos]) = tempw;
        pfq_ip += 2;
        pfq_pos += 2;
    } else if (!is8086 && (pfq_pos < pfq_size)) {
        /* The 8088 fetches 1 byte at a time, and only if there's at least 1 byte
           free in the queue. */
        pfq[pfq_pos] = readmembf(pfq_ip);
        pfq_ip++;
        pfq_pos++;
    }
}

static uint8_t
pfq_read(void)
{
    uint8_t temp = pfq[0];

    for (int i = 0; i < (pfq_size - 1); i++)
        pfq[i] = pfq[i + 1];
    pfq_pos--;
    cpu_state.pc = (cpu_state.pc + 1) & 0xffff;
    return temp;
}

/* Fetches a byte from the prefetch queue, or from memory if the queue has
   been drained. */
static uint8_t
pfq_fetchb_common(void)
{
    uint8_t temp;

    if (pfq_pos == 0) {
        /* Reset prefetch queue internal position. */
        pfq_ip = cpu_state.pc;
        /* Fill the queue. */
        wait_cycs(4 - (biu_cycles & 3), 0);
    }

    /* Fetch. */
    temp = pfq_read();
    return temp;
}

static uint8_t
pfq_fetchb(void)
{
    uint8_t ret;

    ret = pfq_fetchb_common();
    wait_cycs(1, 0);
    return ret;
}

/* Fetches a word from the prefetch queue, or from memory if the queue has
   been drained. */
static uint16_t
pfq_fetchw(void)
{
    uint16_t temp;

    temp = pfq_fetchb_common();
    wait_cycs(1, 0);
    temp |= (pfq_fetchb_common() << 8);

    return temp;
}

static uint16_t
pfq_fetch(void)
{
    if (opcode & 1)
        return pfq_fetchw();
    else
        return (uint16_t) pfq_fetchb();
}

/* Adds bytes to the prefetch queue based on the instruction's cycle count. */
static void
pfq_add(int c, int add)
{
    int d;

    if ((c <= 0) || (pfq_pos >= pfq_size))
        return;

    for (d = 0; d < c; d++) {
        biu_cycles = (biu_cycles + 1) & 0x03;
        if (prefetching && add && (biu_cycles == 0x00))
            pfq_write();
    }
}

/* Clear the prefetch queue - called on reset and on anything that affects either CS or IP. */
static void
pfq_clear(void)
{
    pfq_pos     = 0;
    prefetching = 0;
}

static void
load_cs(uint16_t seg)
{
    cpu_state.seg_cs.base = seg << 4;
    cpu_state.seg_cs.seg  = seg & 0xffff;
}

static void
load_seg(uint16_t seg, x86seg *s)
{
    s->base = seg << 4;
    s->seg  = seg & 0xffff;
}

uint8_t fetch_i8080_opcode(UNUSED(void* priv), uint16_t addr)
{
    return readmemb(cs + addr);
}

uint8_t fetch_i8080_data(UNUSED(void* priv), uint16_t addr)
{
    return readmemb(ds + addr);
}

void put_i8080_data(UNUSED(void* priv), uint16_t addr, uint8_t val)
{
    writememb(ds, addr, val);
}

static uint8_t i8080_port_in(UNUSED(void* priv), uint8_t port)
{
    cpu_io(8, 0, port);
    return AL;
}

static void i8080_port_out(UNUSED(void* priv), uint8_t port, uint8_t val)
{
    AL = val;
    cpu_io(8, 1, port);
}

void
reset_808x(int hard)
{
    biu_cycles = 0;
    in_rep     = 0;
    completed  = 1;
    repeating  = 0;
    clear_lock = 0;
    refresh    = 0;
    ovr_seg    = NULL;

    if (hard) {
        opseg[0]  = &es;
        opseg[1]  = &cs;
        opseg[2]  = &ss;
        opseg[3]  = &ds;
        _opseg[0] = &cpu_state.seg_es;
        _opseg[1] = &cpu_state.seg_cs;
        _opseg[2] = &cpu_state.seg_ss;
        _opseg[3] = &cpu_state.seg_ds;

        pfq_size = (is8086) ? 6 : 4;
    }

    pfq_clear();

    load_cs(0xFFFF);
    cpu_state.pc = 0;
    if (is_nec)
        cpu_state.flags |= MD_FLAG;
    rammask = 0xfffff;

    prefetching = 1;
    cpu_alu_op  = 0;

    use_custom_nmi_vector = 0x00;
    custom_nmi_vector     = 0x00000000;

    cpu_md_write_disable = 1;
    i8080_init(&emulated_processor);
    emulated_processor.write_byte    = put_i8080_data;
    emulated_processor.read_byte     = fetch_i8080_data;
    emulated_processor.read_byte_seg = fetch_i8080_opcode;
    emulated_processor.port_in       = i8080_port_in;
    emulated_processor.port_out      = i8080_port_out;
}

static void
set_ip(uint16_t new_ip)
{
    pfq_ip = cpu_state.pc = new_ip;
    prefetching           = 1;
}

/* Memory refresh read - called by reads and writes on DMA channel 0. */
void
refreshread(void)
{
    refresh++;
}

static uint16_t
get_accum(int bits)
{
    return (bits == 16) ? AX : AL;
}

static void
set_accum(int bits, uint16_t val)
{
    if (bits == 16)
        AX = val;
    else
        AL = val;
}

static uint16_t
sign_extend(uint8_t data)
{
    return data + (data < 0x80 ? 0 : 0xff00);
}

/* Fetches the effective address from the prefetch queue according to MOD and R/M. */
static void
do_mod_rm(void)
{
    rmdat   = pfq_fetchb();
    cpu_reg = (rmdat >> 3) & 7;
    cpu_mod = (rmdat >> 6) & 3;
    cpu_rm  = rmdat & 7;

    if (cpu_mod == 3)
        return;

    wait_cycs(1, 0);
    if ((rmdat & 0xc7) == 0x06) {
        wait_cycs(1, 0);
        cpu_state.eaaddr = pfq_fetchw();
        easeg            = ovr_seg ? *ovr_seg : ds;
        wait_cycs(1, 0);
        return;
    } else
        switch (cpu_rm) {
            case 0:
            case 3:
                wait_cycs(2, 0);
                break;
            case 1:
            case 2:
                wait_cycs(3, 0);
                break;
        }
    cpu_state.eaaddr = (*mod1add[0][cpu_rm]) + (*mod1add[1][cpu_rm]);
    easeg            = ovr_seg ? *ovr_seg : *mod1seg[cpu_rm];
    switch (rmdat & 0xc0) {
        case 0x40:
            wait_cycs(3, 0);
            cpu_state.eaaddr += sign_extend(pfq_fetchb());
            break;
        case 0x80:
            wait_cycs(3, 0);
            cpu_state.eaaddr += pfq_fetchw();
            break;
    }
    cpu_state.eaaddr &= 0xffff;
    wait_cycs(2, 0);
}

#undef getr8
#define getr8(r) ((r & 4) ? cpu_state.regs[r & 3].b.h : cpu_state.regs[r & 3].b.l)

#undef setr8
#define setr8(r, v)                    \
    if (r & 4)                         \
        cpu_state.regs[r & 3].b.h = v; \
    else                               \
        cpu_state.regs[r & 3].b.l = v;

/* Reads a byte from the effective address. */
static uint8_t
geteab(void)
{
    if (cpu_mod == 3)
        return (getr8(cpu_rm));

    return readmemb(easeg + cpu_state.eaaddr);
}

/* Reads a word from the effective address. */
static uint16_t
geteaw(void)
{
    if (cpu_mod == 3)
        return cpu_state.regs[cpu_rm].w;

    return readmemw(easeg, cpu_state.eaaddr);
}

/* Neede for 8087 - memory only. */
static uint32_t
geteal(void)
{
    if (cpu_mod == 3) {
        fatal("808x register geteal()\n");
        return 0xffffffff;
    }

    return readmeml(easeg, cpu_state.eaaddr);
}

/* Neede for 8087 - memory only. */
static uint64_t
geteaq(void)
{
    if (cpu_mod == 3) {
        fatal("808x register geteaq()\n");
        return 0xffffffff;
    }

    return readmemq(easeg, cpu_state.eaaddr);
}

static void
read_ea(int memory_only, int bits)
{
    if (cpu_mod != 3) {
        if (bits == 16)
            cpu_data = readmemw(easeg, cpu_state.eaaddr);
        else
            cpu_data = readmemb(easeg + cpu_state.eaaddr);
        return;
    }
    if (!memory_only) {
        if (bits == 8) {
            cpu_data = getr8(cpu_rm);
        } else
            cpu_data = cpu_state.regs[cpu_rm].w;
    }
}

static void
read_ea2(int bits)
{
    cpu_state.eaaddr = (cpu_state.eaaddr + 2) & 0xffff;
    if (bits == 16)
        cpu_data = readmemw(easeg, cpu_state.eaaddr);
    else
        cpu_data = readmemb(easeg + cpu_state.eaaddr);
}

/* Writes a byte to the effective address. */
static void
seteab(uint8_t val)
{
    if (cpu_mod == 3) {
        setr8(cpu_rm, val);
    } else
        writememb(easeg, cpu_state.eaaddr, val);
}

/* Writes a word to the effective address. */
static void
seteaw(uint16_t val)
{
    if (cpu_mod == 3)
        cpu_state.regs[cpu_rm].w = val;
    else
        writememw(easeg, cpu_state.eaaddr, val);
}

static void
seteal(uint32_t val)
{
    if (cpu_mod == 3) {
        fatal("808x register seteal()\n");
        return;
    } else
        writememl(easeg, cpu_state.eaaddr, val);
}

static void
seteaq(uint64_t val)
{
    if (cpu_mod == 3) {
        fatal("808x register seteaq()\n");
        return;
    } else
        writememq(easeg, cpu_state.eaaddr, val);
}

/* Leave out the 686 stuff as it's not needed and
   complicates compiling. */
#define FPU_8087
#define tempc tempc_fpu
#include "x87_sf.h"
#include "x87.h"
#include "x87_ops.h"
#undef tempc
#undef FPU_8087

/* Pushes a word to the stack. */
static void
push(uint16_t *val)
{
    if ((is186 && !is_nec) && (SP == 1)) {
        writememw(ss - 1, 0, *val);
        SP = cpu_state.eaaddr = 0xFFFF;
        return;
    }
    SP -= 2;
    cpu_state.eaaddr = (SP & 0xffff);
    writememw(ss, cpu_state.eaaddr, *val);
}

/* Pops a word from the stack. */
static uint16_t
pop(void)
{
    cpu_state.eaaddr = (SP & 0xffff);
    SP += 2;
    return readmemw(ss, cpu_state.eaaddr);
}

static void
access(int num, UNUSED(int bits))
{
    switch (num) {
        case 0:
        case 61:
        case 63:
        case 64:
        case 67:
        case 69:
        case 71:
        case 72:
        default:
            break;
        case 1:
        case 6:
        case 7:
        case 8:
        case 9:
        case 17:
        case 20:
        case 21:
        case 24:
        case 28:
        case 47:
        case 48:
        case 49:
        case 50:
        case 51:
        case 55:
        case 56:
        case 62:
        case 66:
        case 68:
            wait_cycs(1, 0);
            break;
        case 3:
        case 11:
        case 15:
        case 22:
        case 23:
        case 25:
        case 26:
        case 35:
        case 44:
        case 45:
        case 46:
        case 52:
        case 53:
        case 54:
            wait_cycs(2, 0);
            break;
        case 16:
        case 18:
        case 19:
        case 27:
        case 32:
        case 37:
        case 42:
            wait_cycs(3, 0);
            break;
        case 10:
        case 12:
        case 13:
        case 14:
        case 29:
        case 30:
        case 33:
        case 34:
        case 39:
        case 41:
        case 60:
            wait_cycs(4, 0);
            break;
        case 4:
        case 70:
            wait_cycs(5, 0);
            break;
        case 31:
        case 38:
        case 40:
            wait_cycs(6, 0);
            break;
        case 5:
            if (opcode == 0xcc)
                wait_cycs(7, 0);
            else
                wait_cycs(4, 0);
            break;
        case 36:
            wait_cycs(1, 0);
            pfq_clear();
            wait_cycs(1, 0);
            if (cpu_mod != 3)
                wait_cycs(1, 0);
            wait_cycs(3, 0);
            break;
        case 43:
            wait_cycs(2, 0);
            pfq_clear();
            wait_cycs(1, 0);
            break;
        case 57:
            if (cpu_mod != 3)
                wait_cycs(2, 0);
            wait_cycs(4, 0);
            break;
        case 58:
            if (cpu_mod != 3)
                wait_cycs(1, 0);
            wait_cycs(4, 0);
            break;
        case 59:
            wait_cycs(2, 0);
            pfq_clear();
            if (cpu_mod != 3)
                wait_cycs(1, 0);
            wait_cycs(3, 0);
            break;
        case 65:
            wait_cycs(1, 0);
            pfq_clear();
            wait_cycs(2, 0);
            if (cpu_mod != 3)
                wait_cycs(1, 0);
            break;
    }
}

/* Calls an interrupt. */
static void
interrupt(uint16_t addr)
{
    uint16_t old_cs, old_ip;
    uint16_t new_cs, new_ip;
    uint16_t tempf;

    if (!(cpu_state.flags & MD_FLAG) && is_nec) {
        sync_from_i8080();
        x808x_log("CALLN/INT#/NMI#\n");
    }

    addr <<= 2;
    cpu_state.eaaddr = addr;
    old_cs           = CS;
    access(5, 16);
    new_ip = readmemw(0, cpu_state.eaaddr);
    wait_cycs(1, 0);
    cpu_state.eaaddr = (cpu_state.eaaddr + 2) & 0xffff;
    access(6, 16);
    new_cs      = readmemw(0, cpu_state.eaaddr);
    prefetching = 0;
    pfq_clear();
    ovr_seg = NULL;
    access(39, 16);
    tempf = cpu_state.flags & (is_nec ? 0x8fd7 : 0x0fd7);
    push(&tempf);
    cpu_state.flags &= ~(I_FLAG | T_FLAG);
    if (is_nec)
        cpu_state.flags |= MD_FLAG;
    access(40, 16);
    push(&old_cs);
    old_ip = cpu_state.pc;
    load_cs(new_cs);
    access(68, 16);
    set_ip(new_ip);
    access(41, 16);
    push(&old_ip);
}

/* Ditto, but for breaking into emulation mode. */
static void
interrupt_brkem(uint16_t addr)
{
    uint16_t old_cs, old_ip;
    uint16_t new_cs, new_ip;
    uint16_t tempf;

    addr <<= 2;
    cpu_state.eaaddr = addr;
    old_cs           = CS;
    access(5, 16);
    new_ip = readmemw(0, cpu_state.eaaddr);
    wait_cycs(1, 0);
    cpu_state.eaaddr = (cpu_state.eaaddr + 2) & 0xffff;
    access(6, 16);
    new_cs      = readmemw(0, cpu_state.eaaddr);
    prefetching = 0;
    pfq_clear();
    ovr_seg = NULL;
    access(39, 16);
    tempf = cpu_state.flags & (is_nec ? 0x8fd7 : 0x0fd7);
    push(&tempf);
    cpu_state.flags      &= ~(MD_FLAG);
    cpu_md_write_disable  = 0;
    access(40, 16);
    push(&old_cs);
    old_ip = cpu_state.pc;
    load_cs(new_cs);
    access(68, 16);
    set_ip(new_ip);
    access(41, 16);
    push(&old_ip);
    sync_to_i8080();
    x808x_log("BRKEM mode\n");
}

void
retem_i8080(void)
{
    sync_from_i8080();

    prefetching = 0;
    pfq_clear();

    set_ip(pop());
    load_cs(pop());
    cpu_state.flags = pop();

    emulated_processor.iff = !!(cpu_state.flags & I_FLAG);

    cpu_md_write_disable = 1;

    noint      = 1;
    nmi_enable = 1;

    x808x_log("RETEM mode\n");
}

void
interrupt_808x(uint16_t addr)
{
    interrupt(addr);
}

static void
custom_nmi(void)
{
    uint16_t old_cs, old_ip;
    uint16_t new_cs, new_ip;
    uint16_t tempf;

    if (!(cpu_state.flags & MD_FLAG) && is_nec) {
        sync_from_i8080();
        pclog("NMI# (CUTSOM)\n");
    }

    cpu_state.eaaddr = 0x0002;
    old_cs           = CS;
    access(5, 16);
    (void) readmemw(0, cpu_state.eaaddr);
    new_ip = custom_nmi_vector & 0xffff;
    wait_cycs(1, 0);
    cpu_state.eaaddr = (cpu_state.eaaddr + 2) & 0xffff;
    access(6, 16);
    (void) readmemw(0, cpu_state.eaaddr);
    new_cs      = custom_nmi_vector >> 16;
    prefetching = 0;
    pfq_clear();
    ovr_seg = NULL;
    access(39, 16);
    tempf = cpu_state.flags & (is_nec ? 0x8fd7 : 0x0fd7);
    push(&tempf);
    cpu_state.flags &= ~(I_FLAG | T_FLAG);
    if (is_nec)
        cpu_state.flags |= MD_FLAG;
    access(40, 16);
    push(&old_cs);
    old_ip = cpu_state.pc;
    load_cs(new_cs);
    access(68, 16);
    set_ip(new_ip);
    access(41, 16);
    push(&old_ip);
}

static int
irq_pending(int nec_hlt)
{
    uint8_t temp;
    int     i_flag = (cpu_state.flags & I_FLAG) || nec_hlt;

    temp = (nmi && nmi_enable && nmi_mask) || ((cpu_state.flags & T_FLAG) && !noint) || (i_flag && pic.int_pending && !noint);

    return temp;
}

static void
check_interrupts(int nec_hlt)
{
    int temp;
    int     i_flag = (cpu_state.flags & I_FLAG) || nec_hlt;

    if (irq_pending(nec_hlt)) {
        if ((cpu_state.flags & T_FLAG) && !(noint & 1)) {
            interrupt(1);
            return;
        }
        if (nmi && nmi_enable && nmi_mask) {
            nmi_enable = 0;
            if (use_custom_nmi_vector)
                custom_nmi();
            else
                interrupt(2);
#ifndef OLD_NMI_BEHAVIOR
            nmi = 0;
#endif
            return;
        }
        if (i_flag && pic.int_pending && !noint) {
            repeating = 0;
            completed = 1;
            ovr_seg   = NULL;
            wait_cycs(3, 0);
            /* ACK to PIC */
            temp = pic_irq_ack();
            wait_cycs(4, 1);
            wait_cycs(1, 0);
            /* ACK to PIC */
            temp = pic_irq_ack();
            wait_cycs(4, 1);
            wait_cycs(1, 0);
            in_lock    = 0;
            clear_lock = 0;
            wait_cycs(1, 0);
            /* Here is where temp should be filled, but we cheat. */
            wait_cycs(3, 0);
            opcode = 0x00;
            interrupt(temp);
        }
    }
}

static int
rep_action(int bits)
{
    uint16_t t;

    if (in_rep == 0)
        return 0;
    wait_cycs(2, 0);
    t = CX;
    if (irq_pending(0) && (repeating != 0)) {
        access(71, bits);
        pfq_clear();
        if (is_nec && (ovr_seg != NULL))
            set_ip(cpu_state.pc - 3);
        else
            set_ip(cpu_state.pc - 2);
        t = 0;
    }
    if (t == 0) {
        wait_cycs(1, 0);
        completed = 1;
        repeating = 0;
        return 1;
    }
    --CX;
    completed = 0;
    wait_cycs(2, 0);
    if (!repeating)
        wait_cycs(2, 0);
    return 0;
}

static uint16_t
jump(uint16_t delta)
{
    uint16_t old_ip;
    access(67, 8);
    pfq_clear();
    wait_cycs(5, 0);
    old_ip = cpu_state.pc;
    set_ip((cpu_state.pc + delta) & 0xffff);
    return old_ip;
}

static void
jump_short(void)
{
    jump(sign_extend((uint8_t) cpu_data));
}

static uint16_t
jump_near(void)
{
    return jump(pfq_fetchw());
}

/* Performs a conditional jump. */
static void
jcc(uint8_t opcode, int cond)
{
    /* int8_t offset; */

    wait_cycs(1, 0);
    cpu_data = pfq_fetchb();
    wait_cycs(1, 0);
    if ((!cond) == !!(opcode & 0x01))
        jump_short();
}

static void
set_cf(int cond)
{
    cpu_state.flags = (cpu_state.flags & ~C_FLAG) | (cond ? C_FLAG : 0);
}

static void
set_if(int cond)
{
    cpu_state.flags = (cpu_state.flags & ~I_FLAG) | (cond ? I_FLAG : 0);
}

static void
set_df(int cond)
{
    cpu_state.flags = (cpu_state.flags & ~D_FLAG) | (cond ? D_FLAG : 0);
}

static void
bitwise(int bits, uint16_t data)
{
    cpu_data = data;
    cpu_state.flags &= ~(C_FLAG | A_FLAG | V_FLAG);
    set_pzs(bits);
}

static void
test(int bits, uint16_t dest, uint16_t src)
{
    cpu_dest = dest;
    cpu_src  = src;
    bitwise(bits, (cpu_dest & cpu_src));
}

static void
set_of(int of)
{
    cpu_state.flags = (cpu_state.flags & ~0x800) | (of ? 0x800 : 0);
}

static int
top_bit(uint16_t w, int bits)
{
    return (w & (1 << (bits - 1)));
}

static void
set_of_add(int bits)
{
    set_of(top_bit((cpu_data ^ cpu_src) & (cpu_data ^ cpu_dest), bits));
}

static void
set_of_sub(int bits)
{
    set_of(top_bit((cpu_dest ^ cpu_src) & (cpu_data ^ cpu_dest), bits));
}

static void
set_af(int af)
{
    cpu_state.flags = (cpu_state.flags & ~0x10) | (af ? 0x10 : 0);
}

static void
do_af(void)
{
    set_af(((cpu_data ^ cpu_src ^ cpu_dest) & 0x10) != 0);
}

static void
set_apzs(int bits)
{
    set_pzs(bits);
    do_af();
}

static void
add(int bits)
{
    int size_mask = (1 << bits) - 1;
    int special_case = 0;
    uint32_t temp_src = cpu_src;

    if ((cpu_alu_op == 2) && !(cpu_src & size_mask) && (cpu_state.flags & C_FLAG))
        special_case = 1;

    cpu_data = cpu_dest + cpu_src;
    if ((cpu_alu_op == 2) && (cpu_state.flags & C_FLAG))
        cpu_src--;
    set_apzs(bits);
    set_of_add(bits);

    /* Anything - FF with carry on is basically anything + 0x100: value stays
       unchanged but carry goes on. */
    if (special_case)
        cpu_state.flags |= C_FLAG;
    else
        set_cf((temp_src & size_mask) > (cpu_data & size_mask));
}

static void
sub(int bits)
{
    int size_mask = (1 << bits) - 1;
    int special_case = 0;
    uint32_t temp_src = cpu_src;

    if ((cpu_alu_op == 3) && !(cpu_src & size_mask) && (cpu_state.flags & C_FLAG))
        special_case = 1;

    cpu_data = cpu_dest - cpu_src;
    if ((cpu_alu_op == 3) && (cpu_state.flags & C_FLAG))
        cpu_src--;
    set_apzs(bits);
    set_of_sub(bits);

    /* Anything - FF with carry on is basically anything - 0x100: value stays
       unchanged but carry goes on. */
    if (special_case)
        cpu_state.flags |= C_FLAG;
    else
        set_cf((temp_src & size_mask) > (cpu_dest & size_mask));
}

static void
alu_op(int bits)
{
    switch (cpu_alu_op) {
        case 1:
            bitwise(bits, (cpu_dest | cpu_src));
            break;
        case 2:
            if (cpu_state.flags & C_FLAG)
                cpu_src++;
            /* Fall through. */
        case 0:
            add(bits);
            break;
        case 3:
            if (cpu_state.flags & C_FLAG)
                cpu_src++;
            /* Fall through. */
        case 5:
        case 7:
            sub(bits);
            break;
        case 4:
            test(bits, cpu_dest, cpu_src);
            break;
        case 6:
            bitwise(bits, (cpu_dest ^ cpu_src));
            break;
    }
}

static void
set_sf(int bits)
{
    cpu_state.flags = (cpu_state.flags & ~0x80) | (top_bit(cpu_data, bits) ? 0x80 : 0);
}

static void
set_pf(void)
{
    cpu_state.flags = (cpu_state.flags & ~4) | (!__builtin_parity(cpu_data & 0xFF) << 2);
}

static void
mul(uint16_t a, uint16_t b)
{
    int      negate    = 0;
    int      bit_count = 8;
    int      carry, i;
    uint16_t high_bit = 0x80;
    uint16_t size_mask;
    uint16_t c, r;

    size_mask = (1 << bit_count) - 1;

    if (opcode != 0xd5) {
        if (opcode & 1) {
            bit_count = 16;
            high_bit  = 0x8000;
        } else
            wait_cycs(8, 0);

        size_mask = (1 << bit_count) - 1;

        if ((rmdat & 0x38) == 0x28) {
            if (!top_bit(a, bit_count)) {
                if (top_bit(b, bit_count)) {
                    wait_cycs(1, 0);
                    if ((b & size_mask) != ((opcode & 1) ? 0x8000 : 0x80))
                        wait_cycs(1, 0);
                    b      = ~b + 1;
                    negate = 1;
                }
            } else {
                wait_cycs(1, 0);
                a      = ~a + 1;
                negate = 1;
                if (top_bit(b, bit_count)) {
                    b      = ~b + 1;
                    negate = 0;
                } else
                    wait_cycs(4, 0);
            }
            wait_cycs(10, 0);
        }
        wait_cycs(3, 0);
    }

    c = 0;
    a &= size_mask;
    carry = (a & 1) != 0;
    a >>= 1;
    for (i = 0; i < bit_count; ++i) {
        wait_cycs(7, 0);
        if (carry) {
            cpu_src  = c;
            cpu_dest = b;
            add(bit_count);
            c = cpu_data & size_mask;
            wait_cycs(1, 0);
            carry = !!(cpu_state.flags & C_FLAG);
        }
        r     = (c >> 1) + (carry ? high_bit : 0);
        carry = (c & 1) != 0;
        c     = r;
        r     = (a >> 1) + (carry ? high_bit : 0);
        carry = (a & 1) != 0;
        a     = r;
    }
    if (negate) {
        c = ~c;
        a = (~a + 1) & size_mask;
        if (a == 0)
            ++c;
        wait_cycs(9, 0);
    }
    cpu_data = a;
    cpu_dest = c;

    set_sf(bit_count);
    set_pf();
    set_af(0);
}

static void
set_of_rotate(int bits)
{
    set_of(top_bit(cpu_data ^ cpu_dest, bits));
}

static void
set_zf_ex(int zf)
{
    cpu_state.flags = (cpu_state.flags & ~0x40) | (zf ? 0x40 : 0);
}

static void
set_zf(int bits)
{
    int size_mask = (1 << bits) - 1;

    set_zf_ex((cpu_data & size_mask) == 0);
}

static void
set_pzs(int bits)
{
    set_pf();
    set_zf(bits);
    set_sf(bits);
}

static void
set_co_mul(UNUSED(int bits), int carry)
{
    set_cf(carry);
    set_of(carry);
    set_zf_ex(!carry);
    if (!carry)
        wait_cycs(1, 0);
}

/* Was div(), renamed to avoid conflicts with stdlib div(). */
static int
x86_div(uint16_t l, uint16_t h)
{
    int      b, bit_count = 8;
    int      negative          = 0;
    int      dividend_negative = 0;
    int      size_mask, carry;
    uint16_t r;

    if (opcode & 1) {
        l         = AX;
        h         = DX;
        bit_count = 16;
    }

    size_mask = (1 << bit_count) - 1;

    if (opcode != 0xd4) {
        if ((rmdat & 0x38) == 0x38) {
            if (top_bit(h, bit_count)) {
                h = ~h;
                l = (~l + 1) & size_mask;
                if (l == 0)
                    ++h;
                h &= size_mask;
                negative          = 1;
                dividend_negative = 1;
                wait_cycs(4, 0);
            }
            if (top_bit(cpu_src, bit_count)) {
                cpu_src  = ~cpu_src + 1;
                negative = !negative;
            } else
                wait_cycs(1, 0);
            wait_cycs(9, 0);
        }
        wait_cycs(3, 0);
    }
    wait_cycs(8, 0);
    cpu_src &= size_mask;
    if (h >= cpu_src) {
        if (opcode != 0xd4)
            wait_cycs(1, 0);
        interrupt(0);
        return 0;
    }
    if (opcode != 0xd4)
        wait_cycs(1, 0);
    wait_cycs(2, 0);
    carry = 1;
    for (b = 0; b < bit_count; ++b) {
        r     = (l << 1) + (carry ? 1 : 0);
        carry = top_bit(l, bit_count);
        l     = r;
        r     = (h << 1) + (carry ? 1 : 0);
        carry = top_bit(h, bit_count);
        h     = r;
        wait_cycs(8, 0);
        if (carry) {
            carry = 0;
            h -= cpu_src;
            if (b == bit_count - 1)
                wait_cycs(2, 0);
        } else {
            carry = cpu_src > h;
            if (!carry) {
                h -= cpu_src;
                wait_cycs(1, 0);
                if (b == bit_count - 1)
                    wait_cycs(2, 0);
            }
        }
    }
    l = ~((l << 1) + (carry ? 1 : 0));
    if (opcode != 0xd4 && (rmdat & 0x38) == 0x38) {
        wait_cycs(4, 0);
        if (top_bit(l, bit_count)) {
            if (cpu_mod == 3)
                wait_cycs(1, 0);
            interrupt(0);
            return 0;
        }
        wait_cycs(7, 0);
        if (negative)
            l = ~l + 1;
        if (dividend_negative)
            h = ~h + 1;
    }
    if (opcode == 0xd4) {
        AL = h & 0xff;
        AH = l & 0xff;
    } else {
        AH = h & 0xff;
        AL = l & 0xff;
        if (opcode & 1) {
            DX = h;
            AX = l;
        }
    }
    return 1;
}

static uint16_t
string_increment(int bits)
{
    int d = bits >> 3;
    if (cpu_state.flags & D_FLAG)
        cpu_state.eaaddr -= d;
    else
        cpu_state.eaaddr += d;
    cpu_state.eaaddr &= 0xffff;
    return cpu_state.eaaddr;
}

static void
lods(int bits)
{
    cpu_state.eaaddr = SI;
    if (bits == 16)
        cpu_data = readmemw((ovr_seg ? *ovr_seg : ds), cpu_state.eaaddr);
    else
        cpu_data = readmemb((ovr_seg ? *ovr_seg : ds) + cpu_state.eaaddr);
    SI = string_increment(bits);
}

static void
stos(int bits)
{
    cpu_state.eaaddr = DI;
    if (bits == 16)
        writememw(es, cpu_state.eaaddr, cpu_data);
    else
        writememb(es, cpu_state.eaaddr, (uint8_t) (cpu_data & 0xff));
    DI = string_increment(bits);
}

static void
aa(void)
{
    set_pzs(8);
    AL = cpu_data & 0x0f;
    wait_cycs(6, 0);
}

static void
set_ca(void)
{
    set_cf(1);
    set_af(1);
}

static void
clear_ca(void)
{
    set_cf(0);
    set_af(0);
}

static uint16_t
get_ea(void)
{
    if (opcode & 1)
        return geteaw();
    else
        return (uint16_t) geteab();
}

static uint16_t
get_reg(uint8_t reg)
{
    if (opcode & 1)
        return cpu_state.regs[reg].w;
    else
        return (uint16_t) getr8(reg);
}

static void
set_ea(uint16_t val)
{
    if (opcode & 1)
        seteaw(val);
    else
        seteab((uint8_t) (val & 0xff));
}

static void
set_reg(uint8_t reg, uint16_t val)
{
    if (opcode & 1)
        cpu_state.regs[reg].w = val;
    else
        setr8(reg, (uint8_t) (val & 0xff));
}

static void
cpu_data_opff_rm(void)
{
    if (!(opcode & 1)) {
        if (cpu_mod != 3)
            cpu_data |= 0xff00;
        else
            cpu_data = cpu_state.regs[cpu_rm].w;
    }
}

uint8_t
cpu_inb(uint16_t port)
{
    int     old_cycles = cycles;
    uint8_t ret;

    wait_cycs(is_mazovia ? 5 : 4, 1);
    old_cycles = cycles;

    ret = inb(port);

    resub_cycles(old_cycles);

    return ret;
}

uint16_t
cpu_inw(uint16_t port)
{
    int      old_cycles = cycles;
    uint16_t ret;

    wait_cycs(is_mazovia ? 5 : 4, 1);
    if (is8086 && !(port & 1)) {
        old_cycles = cycles;
        ret = inw(port);
    } else {
        wait_cycs(is_mazovia ? 5 : 4, 1);
        old_cycles = cycles;
        ret = inb(port++);
        ret |= (inb(port) << 8);
    }

    resub_cycles(old_cycles);

    return ret;
}

void
cpu_outb(uint16_t port, uint16_t val)
{
    int old_cycles = cycles;

    wait_cycs(is_mazovia ? 5 : 4, 1);
    old_cycles = cycles;

    outb(port, val);

    resub_cycles(old_cycles);
}

void
cpu_outw(uint16_t port, uint16_t val)
{
    int old_cycles = cycles;

    wait_cycs(is_mazovia ? 5 : 4, 1);

    if (is8086 && !(port & 1)) {
        old_cycles = cycles;
        outw(port, val);
    } else {
        wait_cycs(is_mazovia ? 5 : 4, 1);
        old_cycles = cycles;
        outb(port++, val);
        outb(port, val >> 8);
    }

    resub_cycles(old_cycles);
}

/* Executes instructions up to the specified number of cycles. */
void
execx86(int cycs)
{
    uint8_t  temp = 0, temp2, old_af, nests;
    uint8_t  temp_val, temp_al, bit, handled = 0;
    uint8_t  odd, zero, nibbles_count, destcmp;
    uint8_t  destbyte, srcbyte, nibble_result, bit_length;
    uint8_t  bit_offset;
    int8_t   nibble_result_s;
    uint16_t addr, tempw, new_cs, new_ip;
    uint16_t tempw_int, size, tempbp, lowbound;
    uint16_t highbound, regval, orig_sp, wordtopush;
    uint16_t immediate, old_flags;
    int      bits;
    uint32_t dest_seg, i, carry, nibble;
    uint32_t srcseg, byteaddr;

    cycles += cycs;

    while (cycles > 0) {
        clock_start();

        if (is_nec && !(cpu_state.flags & MD_FLAG)) {
            i8080_step(&emulated_processor);
            set_if(emulated_processor.iff);
            cycles -= emulated_processor.cyc;
            emulated_processor.cyc = 0;
            completed = 1;
            goto exec_completed;
        }

        if (!repeating) {
            cpu_state.oldpc = cpu_state.pc;
            opcode          = pfq_fetchb();
            handled         = 0;
            oldc            = cpu_state.flags & C_FLAG;
            if (clear_lock) {
                in_lock    = 0;
                clear_lock = 0;
            }
            wait_cycs(1, 0);
        }

        completed = 1;
        // pclog("[%04X:%04X] Opcode: %02X\n", CS, cpu_state.pc, opcode);
        if (is186) {
            switch (opcode) {
                case 0x60: /*PUSHA/PUSH R*/
                    orig_sp = SP;
                    wait_cycs(1, 0);
                    push(&AX);
                    push(&CX);
                    push(&DX);
                    push(&BX);
                    push(&orig_sp);
                    push(&BP);
                    push(&SI);
                    push(&DI);
                    handled = 1;
                    break;
                case 0x61: /*POPA/POP R*/
                    wait_cycs(9, 0);
                    DI = pop();
                    SI = pop();
                    BP = pop();
                    (void) pop(); /* former orig_sp */
                    BX      = pop();
                    DX      = pop();
                    CX      = pop();
                    AX      = pop();
                    handled = 1;
                    break;

                case 0x62: /* BOUND r/m */
                    lowbound  = 0;
                    highbound = 0;
                    regval    = 0;
                    do_mod_rm();

                    lowbound  = readmemw(easeg, cpu_state.eaaddr);
                    highbound = readmemw(easeg, cpu_state.eaaddr + 2);
                    regval    = get_reg(cpu_reg);
                    if (lowbound > regval || highbound < regval) {
                        cpu_state.pc = cpu_state.oldpc;
                        interrupt(5);
                    }
                    handled = 1;
                    break;

                case 0x64:
                case 0x65:
                    if (is_nec) {
                        /* REPC/REPNC */
                        wait_cycs(1, 0);
                        in_rep     = (opcode == 0x64 ? 1 : 2);
                        rep_c_flag = 1;
                        completed  = 0;
                        handled    = 1;
                    }
                    break;

                case 0x68:
                    wordtopush = pfq_fetchw();
                    wait_cycs(1, 0);
                    push(&wordtopush);
                    handled = 1;
                    break;

                case 0x69:
                    immediate = 0;
                    bits      = 16;
                    do_mod_rm();
                    read_ea(0, 16);
                    immediate = pfq_fetchw();
                    mul(cpu_data & 0xFFFF, immediate);
                    set_reg(cpu_reg, cpu_data);
                    set_co_mul(16, cpu_dest != 0);
                    handled = 1;
                    break;

                case 0x6a:
                    wordtopush = sign_extend(pfq_fetchb());
                    push(&wordtopush);
                    handled = 1;
                    break;

                case 0x6b: /* IMUL reg16,reg16/mem16,imm8 */
                    immediate = 0;
                    bits      = 16;
                    do_mod_rm();
                    read_ea(0, 16);
                    immediate = pfq_fetchb();
                    mul(cpu_data & 0xFFFF, immediate);
                    set_reg(cpu_reg, cpu_data);
                    set_co_mul(16, cpu_dest != 0);
                    handled = 1;
                    break;

                case 0x6c:
                case 0x6d: /* INM dst, DW/INS dst, DX */
                    bits    = 8 << (opcode & 1);
                    handled = 1;
                    if (!repeating)
                        wait_cycs(2, 0);

                    if (rep_action(bits))
                        break;
                    else if (!repeating)
                        wait_cycs(7, 0);

                    if (bits == 16) {
                        writememw(es, DI, cpu_inw(DX));
                        DI += (cpu_state.flags & D_FLAG) ? -2 : 2;
                    } else {
                        writememb(es, DI, cpu_inb(DX));
                        DI += (cpu_state.flags & D_FLAG) ? -1 : 1;
                    }

                    if (in_rep == 0)
                        break;

                    repeating = 1;
                    clock_end();
                    break;

                case 0x6e:
                case 0x6f: /* OUTM DW, src/OUTS DX, src */
                    dest_seg = ovr_seg ? *ovr_seg : ds;
                    bits     = 8 << (opcode & 1);
                    handled  = 1;
                    if (!repeating)
                        wait_cycs(2, 0);

                    if (rep_action(bits))
                        break;
                    else if (!repeating)
                        wait_cycs(7, 0);

                    if (bits == 16) {
                        cpu_outw(DX, readmemw(dest_seg, SI));
                        SI += (cpu_state.flags & D_FLAG) ? -2 : 2;
                    } else {
                        cpu_outb(DX, readmemb(dest_seg + SI));
                        SI += (cpu_state.flags & D_FLAG) ? -1 : 1;
                    }
                    if (in_rep == 0)
                        break;

                    repeating = 1;
                    clock_end();
                    break;

                case 0xc8: /* ENTER/PREPARE */
                    tempw_int = 0;
                    size      = pfq_fetchw();
                    nests     = pfq_fetchb();
                    i         = 0;

                    push(&BP);
                    tempw_int = SP;
                    if (nests > 0) {
                        while (--nests) {
                            tempbp = 0;
                            BP -= 2;
                            tempbp = readmemw(ss, BP);
                            push(&tempbp);
                        }
                        push(&tempw_int);
                    }
                    BP = tempw_int;
                    SP -= size;
                    handled = 1;
                    break;

                case 0xc0:
                case 0xc1: /*rot imm8 */
                    bits = 8 << (opcode & 1);
                    do_mod_rm();
                    if (cpu_mod == 3)
                        wait_cycs(1, 0);
                    access(53, bits);
                    cpu_data = get_ea();
                    cpu_src  = pfq_fetchb();

                    wait_cycs((cpu_mod != 3) ? 9 : 6, 0);

                    if (!is_nec)
                        cpu_src &= 0x1F;
                    while (cpu_src != 0) {
                        cpu_dest = cpu_data;
                        oldc     = cpu_state.flags & C_FLAG;
                        switch (rmdat & 0x38) {
                            case 0x00: /* ROL */
                                set_cf(top_bit(cpu_data, bits));
                                cpu_data <<= 1;
                                cpu_data |= ((cpu_state.flags & C_FLAG) ? 1 : 0);
                                set_of_rotate(bits);
                                set_af(0);
                                break;
                            case 0x08: /* ROR */
                                set_cf((cpu_data & 1) != 0);
                                cpu_data >>= 1;
                                if (cpu_state.flags & C_FLAG)
                                    cpu_data |= (!(opcode & 1) ? 0x80 : 0x8000);
                                set_of_rotate(bits);
                                set_af(0);
                                break;
                            case 0x10: /* RCL */
                                set_cf(top_bit(cpu_data, bits));
                                cpu_data = (cpu_data << 1) | (oldc ? 1 : 0);
                                set_of_rotate(bits);
                                set_af(0);
                                break;
                            case 0x18: /* RCR */
                                set_cf((cpu_data & 1) != 0);
                                cpu_data >>= 1;
                                if (oldc)
                                    cpu_data |= (!(opcode & 0x01) ? 0x80 : 0x8000);
                                set_cf((cpu_dest & 1) != 0);
                                set_of_rotate(bits);
                                set_af(0);
                                break;
                            case 0x20: /* SHL */
                                set_cf(top_bit(cpu_data, bits));
                                cpu_data <<= 1;
                                set_of_rotate(bits);
                                set_af((cpu_data & 0x10) != 0);
                                set_pzs(bits);
                                break;
                            case 0x28: /* SHR */
                                set_cf((cpu_data & 1) != 0);
                                cpu_data >>= 1;
                                set_of_rotate(bits);
                                set_af(0);
                                set_pzs(bits);
                                break;
                            case 0x30: /* SETMO - undocumented? */
                                bitwise(bits, 0xffff);
                                set_cf(0);
                                set_of_rotate(bits);
                                set_af(0);
                                set_pzs(bits);
                                break;
                            case 0x38: /* SAR */
                                set_cf((cpu_data & 1) != 0);
                                cpu_data >>= 1;
                                if (!(opcode & 1))
                                    cpu_data |= (cpu_dest & 0x80);
                                else
                                    cpu_data |= (cpu_dest & 0x8000);
                                set_of_rotate(bits);
                                set_af(0);
                                set_pzs(bits);
                                break;
                        }
                        if ((opcode & 2) != 0)
                            wait_cycs(4, 0);
                        --cpu_src;
                    }
                    access(17, bits);
                    set_ea(cpu_data);
                    handled = 1;
                    break;

                case 0xc9: /* LEAVE/DISPOSE */
                    SP      = BP;
                    BP      = pop();
                    handled = 1;
                    break;
            }
        }
        if (!handled) {
            switch (opcode) {
                case 0x06:
                case 0x0E:
                case 0x16:
                case 0x1E: /* PUSH seg */
                    access(29, 16);
                    push(&(_opseg[(opcode >> 3) & 0x03]->seg));
                    break;
                case 0x07:
                case 0x0F:
                case 0x17:
                case 0x1F: /* POP seg */
                    if (is_nec && (opcode == 0x0F)) {
                        uint8_t orig_opcode = opcode;
                        opcode              = pfq_fetchb();
                        switch (opcode) {
                            case 0x28: /* ROL4 r/m */
                                do_mod_rm();
                                wait_cycs(21, 0);

                                temp_val = geteab();
                                temp_al  = AL;

                                temp_al &= 0xF;
                                temp_al |= (temp_val & 0xF0);
                                temp_val = (temp_al & 0xF) | ((temp_val & 0xF) << 4);
                                temp_al >>= 4;
                                temp_al &= 0xF;
                                seteab(temp_val);
                                AL = temp_al;

                                handled = 1;
                                break;

                            case 0x2a: /* ROR4 r/m */
                                do_mod_rm();
                                wait_cycs(21, 0);

                                temp_val = geteab();
                                temp_al  = AL;

                                AL       = temp_val & 0xF;
                                temp_val = (temp_val >> 4) | ((temp_al & 0xF) << 4);

                                seteab(temp_val);

                                handled = 1;
                                break;

                            case 0x10: /* TEST1 r8/m8, CL*/
                            case 0x11: /* TEST1 r16/m16, CL*/
                            case 0x18: /* TEST1 r8/m8, imm3 */
                            case 0x19: /* TEST1 r16/m16, imm4 */
                                bits = 8 << (opcode & 0x1);
                                do_mod_rm();
                                wait_cycs(3, 0);

                                bit = (opcode & 0x8) ? (pfq_fetchb()) : (CL);
                                bit &= ((1 << (3 + (opcode & 0x1))) - 1);
                                read_ea(0, bits);

                                set_zf_ex(!(cpu_data & (1 << bit)));
                                cpu_state.flags &= ~(V_FLAG | C_FLAG);

                                handled = 1;
                                break;

                            case 0x16: /* NOT1 r8/m8, CL*/
                            case 0x17: /* NOT1 r16/m16, CL*/
                            case 0x1e: /* NOT1 r8/m8, imm3 */
                            case 0x1f: /* NOT1 r16/m16, imm4 */
                                bits = 8 << (opcode & 0x1);
                                do_mod_rm();
                                wait_cycs(3, 0);

                                bit = (opcode & 0x8) ? (pfq_fetchb()) : (CL);
                                bit &= ((1 << (3 + (opcode & 0x1))) - 1);
                                read_ea(0, bits);

                                if (bits == 8)
                                    seteab((cpu_data & 0xFF) ^ (1 << bit));
                                else
                                    seteaw((cpu_data & 0xFFFF) ^ (1 << bit));

                                handled = 1;
                                break;

                            case 0x14: /* SET1 r8/m8, CL*/
                            case 0x15: /* SET1 r16/m16, CL*/
                            case 0x1c: /* SET1 r8/m8, imm3 */
                            case 0x1d: /* SET1 r16/m16, imm4 */
                                bits = 8 << (opcode & 0x1);
                                do_mod_rm();
                                wait_cycs(3, 0);

                                bit = (opcode & 0x8) ? (pfq_fetchb()) : (CL);
                                bit &= ((1 << (3 + (opcode & 0x1))) - 1);
                                read_ea(0, bits);

                                if (bits == 8)
                                    seteab((cpu_data & 0xFF) | (1 << bit));
                                else
                                    seteaw((cpu_data & 0xFFFF) | (1 << bit));

                                handled = 1;
                                break;

                            case 0x12: /* CLR1 r8/m8, CL*/
                            case 0x13: /* CLR1 r16/m16, CL*/
                            case 0x1a: /* CLR1 r8/m8, imm3 */
                            case 0x1b: /* CLR1 r16/m16, imm4 */
                                bits = 8 << (opcode & 0x1);
                                do_mod_rm();
                                wait_cycs(3, 0);

                                bit = (opcode & 0x8) ? (pfq_fetchb()) : (CL);
                                bit &= ((1 << (3 + (opcode & 0x1))) - 1);
                                read_ea(0, bits);

                                if (bits == 8)
                                    seteab((cpu_data & 0xFF) & ~(1 << bit));
                                else
                                    seteaw((cpu_data & 0xFFFF) & ~(1 << bit));

                                handled = 1;
                                break;

                            case 0x20: /* ADD4S */
                                odd           = !!(CL % 2);
                                zero          = 1;
                                nibbles_count = CL - odd;
                                i             = 0;
                                carry         = 0;
                                nibble        = 0;
                                srcseg        = ovr_seg ? *ovr_seg : ds;

                                wait_cycs(5, 0);
                                for (i = 0; i < ((nibbles_count / 2) + odd); i++) {
                                    wait_cycs(19, 0);
                                    destcmp = read_mem_b((es) + DI + i);
                                    for (nibble = 0; nibble < 2; nibble++) {
                                        destbyte = destcmp >> (nibble ? 4 : 0);
                                        srcbyte  = read_mem_b(srcseg + SI + i) >> (nibble ? 4 : 0);
                                        destbyte &= 0xF;
                                        srcbyte &= 0xF;
                                        nibble_result = (i == (nibbles_count / 2) && nibble == 1) ? (destbyte + carry) : ((uint8_t) (destbyte)) + ((uint8_t) (srcbyte)) + ((uint32_t) carry);
                                        carry         = 0;
                                        while (nibble_result >= 10) {
                                            nibble_result -= 10;
                                            carry++;
                                        }
                                        if (zero != 0 || (i == (nibbles_count / 2) && nibble == 1))
                                            zero = (nibble_result == 0);
                                        destcmp = ((destcmp & (nibble ? 0x0F : 0xF0)) | (nibble_result << (4 * nibble)));
                                    }
                                    write_mem_b(es + DI + i, destcmp);
                                }
                                set_cf(!!carry);
                                set_zf(!!zero);
                                handled = 1;
                                break;

                            case 0x22: /* SUB4S */
                                odd           = !!(CL % 2);
                                zero          = 1;
                                nibbles_count = CL - odd;
                                i             = 0;
                                carry         = 0;
                                nibble        = 0;
                                srcseg        = ovr_seg ? *ovr_seg : ds;

                                wait_cycs(5, 0);
                                for (i = 0; i < ((nibbles_count / 2) + odd); i++) {
                                    wait_cycs(19, 0);
                                    destcmp = read_mem_b((es) + DI + i);
                                    for (nibble = 0; nibble < 2; nibble++) {
                                        destbyte = destcmp >> (nibble ? 4 : 0);
                                        srcbyte  = read_mem_b(srcseg + SI + i) >> (nibble ? 4 : 0);
                                        destbyte &= 0xF;
                                        srcbyte &= 0xF;
                                        nibble_result_s = (i == (nibbles_count / 2) && nibble == 1) ? ((int8_t) destbyte - (int8_t) carry) : ((int8_t) (destbyte)) - ((int8_t) (srcbyte)) - ((int8_t) carry);
                                        carry           = 0;
                                        while (nibble_result_s < 0) {
                                            nibble_result_s += 10;
                                            carry++;
                                        }
                                        if (zero != 0 || (i == (nibbles_count / 2) && nibble == 1))
                                            zero = (nibble_result_s == 0);
                                        destcmp = ((destcmp & (nibble ? 0x0F : 0xF0)) | (nibble_result_s << (4 * nibble)));
                                    }
                                    write_mem_b(es + DI + i, destcmp);
                                }
                                set_cf(!!carry);
                                set_zf(!!zero);
                                handled = 1;
                                break;

                            case 0x26: /* CMP4S */
                                odd           = !!(CL % 2);
                                zero          = 1;
                                nibbles_count = CL - odd;
                                i             = 0;
                                carry         = 0;
                                nibble        = 0;
                                srcseg        = ovr_seg ? *ovr_seg : ds;

                                wait_cycs(5, 0);
                                for (i = 0; i < ((nibbles_count / 2) + odd); i++) {
                                    wait_cycs(19, 0);
                                    destcmp = read_mem_b((es) + DI + i);
                                    for (nibble = 0; nibble < 2; nibble++) {
                                        destbyte = destcmp >> (nibble ? 4 : 0);
                                        srcbyte  = read_mem_b(srcseg + SI + i) >> (nibble ? 4 : 0);
                                        destbyte &= 0xF;
                                        srcbyte &= 0xF;
                                        nibble_result_s = ((int8_t) (destbyte)) - ((int8_t) (srcbyte)) - ((int8_t) carry);
                                        carry           = 0;
                                        while (nibble_result_s < 0) {
                                            nibble_result_s += 10;
                                            carry++;
                                        }
                                        if (zero != 0 || (i == (nibbles_count / 2) && nibble == 1))
                                            zero = (nibble_result_s == 0);
                                        destcmp = ((destcmp & (nibble ? 0x0F : 0xF0)) | (nibble_result_s << (4 * nibble)));
                                    }
                                }
                                set_cf(!!carry);
                                set_zf(!!zero);
                                handled = 1;
                                break;

                            case 0x31: /* INS reg1, reg2 */
                            case 0x39: /* INS reg8, imm4 */
                                do_mod_rm();
                                wait_cycs(1, 0);

                                bit_length = ((opcode & 0x8) ? (pfq_fetchb() & 0xF) : (getr8(cpu_reg) & 0xF)) + 1;
                                bit_offset = getr8(cpu_rm) & 0xF;
                                byteaddr   = (es) + DI;
                                i          = 0;

                                if (bit_offset >= 8) {
                                    DI++;
                                    byteaddr++;
                                    bit_offset -= 8;
                                }
                                for (i = 0; i < bit_length; i++) {
                                    byteaddr = (es) + DI;
                                    writememb(es, DI, (read_mem_b(byteaddr) & ~(1 << (bit_offset))) | ((!!(AX & (1 << i))) << bit_offset));
                                    bit_offset++;
                                    if (bit_offset == 8) {
                                        DI++;
                                        bit_offset = 0;
                                    }
                                }
                                setr8(cpu_rm, bit_offset);

                                handled = 1;
                                break;

                            case 0x33: /* EXT reg1, reg2 */
                            case 0x3b: /* EXT reg8, imm4 */
                                do_mod_rm();
                                wait_cycs(1, 0);

                                bit_length = ((opcode & 0x8) ? (pfq_fetchb() & 0xF) : (getr8(cpu_reg) & 0xF)) + 1;
                                bit_offset = getr8(cpu_rm) & 0xF;
                                byteaddr   = (ds) + SI;
                                i          = 0;

                                if (bit_offset >= 8) {
                                    SI++;
                                    byteaddr++;
                                    bit_offset -= 8;
                                }

                                AX = 0;
                                for (i = 0; i < bit_length; i++) {
                                    byteaddr = (ds) + SI;
                                    AX |= (!!(readmemb(byteaddr) & (1 << bit_offset))) << i;
                                    bit_offset++;
                                    if (bit_offset == 8) {
                                        SI++;
                                        bit_offset = 0;
                                    }
                                }
                                setr8(cpu_rm, bit_offset);

                                handled = 1;
                                break;

                            case 0xFF: /* BRKEM */
                                interrupt_brkem(pfq_fetchb());
                                handled = 1;
                                break;

                            default:
                                opcode = orig_opcode;
                                cpu_state.pc--;
                                break;
                        }
                    } else
                        handled = 0;
                    if (handled)
                        break;
                    access(22, 16);
                    if (opcode == 0x0F) {
                        load_cs(pop());
                        pfq_pos = 0;
                    } else
                        load_seg(pop(), _opseg[(opcode >> 3) & 0x03]);
                    wait_cycs(1, 0);
                    /* All POP segment instructions suppress interrupts for one instruction. */
                    noint = 1;
                    break;

                case 0x26: /*ES:*/
                case 0x2E: /*CS:*/
                case 0x36: /*SS:*/
                case 0x3E: /*DS:*/
                    wait_cycs(1, 0);
                    ovr_seg   = opseg[(opcode >> 3) & 0x03];
                    completed = 0;
                    break;

                case 0x00:
                case 0x01:
                case 0x02:
                case 0x03:
                case 0x08:
                case 0x09:
                case 0x0a:
                case 0x0b:
                case 0x10:
                case 0x11:
                case 0x12:
                case 0x13:
                case 0x18:
                case 0x19:
                case 0x1a:
                case 0x1b:
                case 0x20:
                case 0x21:
                case 0x22:
                case 0x23:
                case 0x28:
                case 0x29:
                case 0x2a:
                case 0x2b:
                case 0x30:
                case 0x31:
                case 0x32:
                case 0x33:
                case 0x38:
                case 0x39:
                case 0x3a:
                case 0x3b:
                    /* alu rm, r / r, rm */
                    bits = 8 << (opcode & 1);
                    do_mod_rm();
                    access(46, bits);
                    tempw      = get_ea();
                    cpu_alu_op = (opcode >> 3) & 7;
                    if ((opcode & 2) == 0) {
                        cpu_dest = tempw;
                        cpu_src  = get_reg(cpu_reg);
                    } else {
                        cpu_dest = get_reg(cpu_reg);
                        cpu_src  = tempw;
                    }
                    if (cpu_mod != 3)
                        wait_cycs(2, 0);
                    wait_cycs(1, 0);
                    alu_op(bits);
                    if (cpu_alu_op != 7) {
                        if ((opcode & 2) == 0) {
                            access(10, bits);
                            set_ea(cpu_data);
                            if (cpu_mod == 3)
                                wait_cycs(1, 0);
                        } else {
                            set_reg(cpu_reg, cpu_data);
                            wait_cycs(1, 0);
                        }
                    } else
                        wait_cycs(1, 0);
                    break;

                case 0x04:
                case 0x05:
                case 0x0c:
                case 0x0d:
                case 0x14:
                case 0x15:
                case 0x1c:
                case 0x1d:
                case 0x24:
                case 0x25:
                case 0x2c:
                case 0x2d:
                case 0x34:
                case 0x35:
                case 0x3c:
                case 0x3d:
                    /* alu A, imm */
                    bits = 8 << (opcode & 1);
                    wait_cycs(1, 0);
                    cpu_data   = pfq_fetch();
                    cpu_dest   = get_accum(bits); /* AX/AL */
                    cpu_src    = cpu_data;
                    cpu_alu_op = (opcode >> 3) & 7;
                    alu_op(bits);
                    if (cpu_alu_op != 7)
                        set_accum(bits, cpu_data);
                    wait_cycs(1, 0);
                    break;

                case 0x27: /*DAA*/
                    cpu_dest = AL;
                    set_of(0);
                    old_af = !!(cpu_state.flags & A_FLAG);
                    if ((cpu_state.flags & A_FLAG) || (AL & 0x0f) > 9) {
                        cpu_src  = 6;
                        cpu_data = cpu_dest + cpu_src;
                        set_of_add(8);
                        cpu_dest = cpu_data;
                        set_af(1);
                    }
                    if ((cpu_state.flags & C_FLAG) || AL > (old_af ? 0x9f : 0x99)) {
                        cpu_src  = 0x60;
                        cpu_data = cpu_dest + cpu_src;
                        set_of_add(8);
                        cpu_dest = cpu_data;
                        set_cf(1);
                    }
                    AL = cpu_dest;
                    set_pzs(8);
                    wait_cycs(3, 0);
                    break;
                case 0x2F: /*DAS*/
                    cpu_dest = AL;
                    set_of(0);
                    old_af = !!(cpu_state.flags & A_FLAG);
                    if ((cpu_state.flags & A_FLAG) || ((AL & 0xf) > 9)) {
                        cpu_src  = 6;
                        cpu_data = cpu_dest - cpu_src;
                        set_of_sub(8);
                        cpu_dest = cpu_data;
                        set_af(1);
                    }
                    if ((cpu_state.flags & C_FLAG) || AL > (old_af ? 0x9f : 0x99)) {
                        cpu_src  = 0x60;
                        cpu_data = cpu_dest - cpu_src;
                        set_of_sub(8);
                        cpu_dest = cpu_data;
                        set_cf(1);
                    }
                    AL = cpu_dest;
                    set_pzs(8);
                    wait_cycs(3, 0);
                    break;
                case 0x37: /*AAA*/
                    wait_cycs(1, 0);
                    if ((cpu_state.flags & A_FLAG) || ((AL & 0xf) > 9)) {
                        cpu_src = 6;
                        ++AH;
                        set_ca();
                    } else {
                        cpu_src = 0;
                        clear_ca();
                        wait_cycs(1, 0);
                    }
                    cpu_dest = AL;
                    cpu_data = cpu_dest + cpu_src;
                    set_of_add(8);
                    aa();
                    break;
                case 0x3F: /*AAS*/
                    wait_cycs(1, 0);
                    if ((cpu_state.flags & A_FLAG) || ((AL & 0xf) > 9)) {
                        cpu_src = 6;
                        --AH;
                        set_ca();
                    } else {
                        cpu_src = 0;
                        clear_ca();
                        wait_cycs(1, 0);
                    }
                    cpu_dest = AL;
                    cpu_data = cpu_dest - cpu_src;
                    set_of_sub(8);
                    aa();
                    break;

                case 0x40:
                case 0x41:
                case 0x42:
                case 0x43:
                case 0x44:
                case 0x45:
                case 0x46:
                case 0x47:
                case 0x48:
                case 0x49:
                case 0x4A:
                case 0x4B:
                case 0x4C:
                case 0x4D:
                case 0x4E:
                case 0x4F:
                    /* INCDEC rw */
                    wait_cycs(1, 0);
                    cpu_dest = cpu_state.regs[opcode & 7].w;
                    cpu_src  = 1;
                    bits     = 16;
                    if ((opcode & 8) == 0) {
                        cpu_data = cpu_dest + cpu_src;
                        set_of_add(bits);
                    } else {
                        cpu_data = cpu_dest - cpu_src;
                        set_of_sub(bits);
                    }
                    do_af();
                    set_pzs(16);
                    cpu_state.regs[opcode & 7].w = cpu_data;
                    break;

                case 0x50:
                case 0x51:
                case 0x52:
                case 0x53: /*PUSH r16*/
                case 0x54:
                case 0x55:
                case 0x56:
                case 0x57:
                    access(30, 16);
                    push(&(cpu_state.regs[opcode & 0x07].w));
                    break;
                case 0x58:
                case 0x59:
                case 0x5A:
                case 0x5B: /*POP r16*/
                case 0x5C:
                case 0x5D:
                case 0x5E:
                case 0x5F:
                    access(23, 16);
                    cpu_state.regs[opcode & 0x07].w = pop();
                    wait_cycs(1, 0);
                    break;

                case 0x60: /*JO alias*/
                case 0x70: /*JO*/
                case 0x61: /*JNO alias*/
                case 0x71: /*JNO*/
                    jcc(opcode, cpu_state.flags & V_FLAG);
                    break;
                case 0x62: /*JB alias*/
                case 0x72: /*JB*/
                case 0x63: /*JNB alias*/
                case 0x73: /*JNB*/
                    jcc(opcode, cpu_state.flags & C_FLAG);
                    break;
                case 0x64: /*JE alias*/
                case 0x74: /*JE*/
                case 0x65: /*JNE alias*/
                case 0x75: /*JNE*/
                    jcc(opcode, cpu_state.flags & Z_FLAG);
                    break;
                case 0x66: /*JBE alias*/
                case 0x76: /*JBE*/
                case 0x67: /*JNBE alias*/
                case 0x77: /*JNBE*/
                    jcc(opcode, cpu_state.flags & (C_FLAG | Z_FLAG));
                    break;
                case 0x68: /*JS alias*/
                case 0x78: /*JS*/
                case 0x69: /*JNS alias*/
                case 0x79: /*JNS*/
                    jcc(opcode, cpu_state.flags & N_FLAG);
                    break;
                case 0x6A: /*JP alias*/
                case 0x7A: /*JP*/
                case 0x6B: /*JNP alias*/
                case 0x7B: /*JNP*/
                    jcc(opcode, cpu_state.flags & P_FLAG);
                    break;
                case 0x6C: /*JL alias*/
                case 0x7C: /*JL*/
                case 0x6D: /*JNL alias*/
                case 0x7D: /*JNL*/
                    temp  = (cpu_state.flags & N_FLAG) ? 1 : 0;
                    temp2 = (cpu_state.flags & V_FLAG) ? 1 : 0;
                    jcc(opcode, temp ^ temp2);
                    break;
                case 0x6E: /*JLE alias*/
                case 0x7E: /*JLE*/
                case 0x6F: /*JNLE alias*/
                case 0x7F: /*JNLE*/
                    temp  = (cpu_state.flags & N_FLAG) ? 1 : 0;
                    temp2 = (cpu_state.flags & V_FLAG) ? 1 : 0;
                    jcc(opcode, (cpu_state.flags & Z_FLAG) || (temp != temp2));
                    break;

                case 0x80:
                case 0x81:
                case 0x82:
                case 0x83:
                    /* alu rm, imm */
                    bits = 8 << (opcode & 1);
                    do_mod_rm();
                    access(47, bits);
                    cpu_data = get_ea();
                    cpu_dest = cpu_data;
                    if (cpu_mod != 3)
                        wait_cycs(3, 0);
                    if (opcode == 0x81) {
                        if (cpu_mod == 3)
                            wait_cycs(1, 0);
                        cpu_src = pfq_fetchw();
                    } else {
                        if (cpu_mod == 3)
                            wait_cycs(1, 0);
                        if (opcode == 0x83)
                            cpu_src = sign_extend(pfq_fetchb());
                        else
                            cpu_src = pfq_fetchb() | 0xff00;
                    }
                    wait_cycs(1, 0);
                    cpu_alu_op = (rmdat & 0x38) >> 3;
                    alu_op(bits);
                    if (cpu_alu_op != 7) {
                        access(11, bits);
                        set_ea(cpu_data);
                    } else {
                        if (cpu_mod != 3)
                            wait_cycs(1, 0);
                    }
                    break;

                case 0x84:
                case 0x85:
                    /* TEST rm, reg */
                    bits = 8 << (opcode & 1);
                    do_mod_rm();
                    access(48, bits);
                    cpu_data = get_ea();
                    test(bits, cpu_data, get_reg(cpu_reg));
                    if (cpu_mod == 3)
                        wait_cycs(2, 0);
                    wait_cycs(2, 0);
                    break;
                case 0x86:
                case 0x87:
                    /* XCHG rm, reg */
                    bits = 8 << (opcode & 1);
                    do_mod_rm();
                    access(49, bits);
                    cpu_data = get_ea();
                    cpu_src  = get_reg(cpu_reg);
                    set_reg(cpu_reg, cpu_data);
                    wait_cycs(3, 0);
                    access(12, bits);
                    set_ea(cpu_src);
                    break;

                case 0x88:
                case 0x89:
                    /* MOV rm, reg */
                    bits = 8 << (opcode & 1);
                    do_mod_rm();
                    wait_cycs(1, 0);
                    access(13, bits);
                    set_ea(get_reg(cpu_reg));
                    break;
                case 0x8A:
                case 0x8B:
                    /* MOV reg, rm */
                    bits = 8 << (opcode & 1);
                    do_mod_rm();
                    access(50, bits);
                    set_reg(cpu_reg, get_ea());
                    wait_cycs(1, 0);
                    if (cpu_mod != 3)
                        wait_cycs(2, 0);
                    break;

                case 0x8C: /*MOV w,sreg*/
                    do_mod_rm();
                    if (cpu_mod == 3)
                        wait_cycs(1, 0);
                    access(14, 16);
                    seteaw(_opseg[(rmdat & 0x18) >> 3]->seg);
                    break;

                case 0x8D: /*LEA*/
                    do_mod_rm();
                    cpu_state.regs[cpu_reg].w = cpu_state.eaaddr;
                    wait_cycs(1, 0);
                    if (cpu_mod != 3)
                        wait_cycs(2, 0);
                    break;

                case 0x8E: /*MOV sreg,w*/
                    do_mod_rm();
                    access(51, 16);
                    tempw = geteaw();
                    if ((rmdat & 0x18) == 0x08) {
                        load_cs(tempw);
                        pfq_pos = 0;
                    } else
                        load_seg(tempw, _opseg[(rmdat & 0x18) >> 3]);
                    wait_cycs(1, 0);
                    if (cpu_mod != 3)
                        wait_cycs(2, 0);
                    if (((rmdat & 0x18) >> 3) == 2)
                        noint = 1;
                    break;

                case 0x8F: /*POPW*/
                    do_mod_rm();
                    wait_cycs(1, 0);
                    cpu_src = cpu_state.eaaddr;
                    access(24, 16);
                    if (cpu_mod != 3)
                        wait_cycs(2, 0);
                    cpu_data         = pop();
                    cpu_state.eaaddr = cpu_src;
                    wait_cycs(2, 0);
                    access(15, 16);
                    seteaw(cpu_data);
                    break;

                case 0x90:
                case 0x91:
                case 0x92:
                case 0x93:
                case 0x94:
                case 0x95:
                case 0x96:
                case 0x97:
                    /* XCHG AX, rw */
                    wait_cycs(1, 0);
                    cpu_data                     = cpu_state.regs[opcode & 7].w;
                    cpu_state.regs[opcode & 7].w = AX;
                    AX                           = cpu_data;
                    wait_cycs(1, 0);
                    break;

                case 0x98: /*CBW*/
                    wait_cycs(1, 0);
                    AX = sign_extend(AL);
                    break;
                case 0x99: /*CWD*/
                    wait_cycs(4, 0);
                    if (!top_bit(AX, 16))
                        DX = 0;
                    else {
                        wait_cycs(1, 0);
                        DX = 0xffff;
                    }
                    break;
                case 0x9A: /*CALL FAR*/
                    wait_cycs(1, 0);
                    new_ip = pfq_fetchw();
                    wait_cycs(1, 0);
                    new_cs = pfq_fetchw();
                    pfq_clear();
                    access(31, 16);
                    push(&(CS));
                    access(60, 16);
                    cpu_state.oldpc = cpu_state.pc;
                    load_cs(new_cs);
                    set_ip(new_ip);
                    access(32, 16);
                    push((uint16_t *) &(cpu_state.oldpc));
                    break;
                case 0x9B: /*WAIT*/
                    if (!repeating)
                        wait_cycs(2, 0);
                    wait_cycs(5, 0);
#ifdef NO_HACK
                    if (irq_pending(0)) {
                        wait_cycs(7, 0);
                        check_interrupts(0);
                    } else {
                        repeating = 1;
                        completed = 0;
                        clock_end();
                    }
#else
                    wait_cycs(7, 0);
                    check_interrupts(0);
#endif
                    break;
                case 0x9C: /*PUSHF*/
                    access(33, 16);
                    if (is_nec)
                        tempw = (cpu_state.flags & 0x8fd7) | 0x7000;
                    else
                        tempw = (cpu_state.flags & 0x0fd7) | 0xf000;
                    push(&tempw);
                    break;
                case 0x9D: { /*POPF*/
                    uint16_t old_flags = cpu_state.flags;
                    access(25, 16);
                    if (is_nec && cpu_md_write_disable)
                        cpu_state.flags = pop() | 0x8002;
                    else
                        cpu_state.flags = pop() | 0x0002;
                    wait_cycs(1, 0);
                    if ((old_flags ^ cpu_state.flags) & T_FLAG)
                        noint = 1;
                    sync_to_i8080();
                    break;
                } case 0x9E: /*SAHF*/
                    wait_cycs(1, 0);
                    cpu_state.flags = (cpu_state.flags & 0xff02) | AH;
                    wait_cycs(2, 0);
                    break;
                case 0x9F: /*LAHF*/
                    wait_cycs(1, 0);
                    AH = cpu_state.flags & 0xd7;
                    break;

                case 0xA0:
                case 0xA1:
                    /* MOV A, [iw] */
                    bits = 8 << (opcode & 1);
                    wait_cycs(1, 0);
                    cpu_state.eaaddr = pfq_fetchw();
                    access(1, bits);
                    set_accum(bits, readmem((ovr_seg ? *ovr_seg : ds)));
                    wait_cycs(1, 0);
                    break;
                case 0xA2:
                case 0xA3:
                    /* MOV [iw], A */
                    bits = 8 << (opcode & 1);
                    wait_cycs(1, 0);
                    cpu_state.eaaddr = pfq_fetchw();
                    access(7, bits);
                    writemem((ovr_seg ? *ovr_seg : ds), get_accum(bits));
                    break;

                case 0xA4:
                case 0xA5: /* MOVS */
                case 0xAC:
                case 0xAD: /* LODS */
                    bits = 8 << (opcode & 1);
                    if (!repeating) {
                        wait_cycs(1, 0);
                        if ((opcode & 8) == 0 && in_rep != 0)
                            wait_cycs(1, 0);
                    }
                    if (rep_action(bits)) {
                        wait_cycs(1, 0);
                        if ((opcode & 8) != 0)
                            wait_cycs(1, 0);
                        break;
                    }
                    if (in_rep != 0 && (opcode & 8) != 0)
                        wait_cycs(1, 0);
                    access(20, bits);
                    lods(bits);
                    if ((opcode & 8) == 0) {
                        access(27, bits);
                        stos(bits);
                    } else {
                        set_accum(bits, cpu_data);
                        if (in_rep != 0)
                            wait_cycs(2, 0);
                    }
                    if (in_rep == 0) {
                        wait_cycs(3, 0);
                        if ((opcode & 8) != 0)
                            wait_cycs(1, 0);
                        break;
                    }
                    repeating = 1;
                    clock_end();
                    break;

                case 0xA6:
                case 0xA7: /* CMPS */
                case 0xAE:
                case 0xAF: /* SCAS */
                    bits = 8 << (opcode & 1);
                    if (!repeating)
                        wait_cycs(1, 0);
                    if (rep_action(bits)) {
                        wait_cycs(2, 0);
                        break;
                    }
                    if (in_rep != 0)
                        wait_cycs(1, 0);
                    wait_cycs(1, 0);
                    cpu_dest = get_accum(bits);
                    if ((opcode & 8) == 0) {
                        access(21, bits);
                        lods(bits);
                        wait_cycs(1, 0);
                        cpu_dest = cpu_data;
                    }
                    access(2, bits);
                    cpu_state.eaaddr = DI;
                    cpu_data         = readmem(es);
                    DI               = string_increment(bits);
                    cpu_src          = cpu_data;
                    sub(bits);
                    wait_cycs(2, 0);
                    if (in_rep == 0) {
                        wait_cycs(3, 0);
                        break;
                    }
                    if ((!!(cpu_state.flags & (rep_c_flag ? C_FLAG : Z_FLAG))) == (in_rep == 1)) {
                        completed = 1;
                        wait_cycs(4, 0);
                        break;
                    }
                    repeating = 1;
                    clock_end();
                    break;

                case 0xA8:
                case 0xA9:
                    /* TEST A, imm */
                    bits = 8 << (opcode & 1);
                    wait_cycs(1, 0);
                    cpu_data = pfq_fetch();
                    test(bits, get_accum(bits), cpu_data);
                    wait_cycs(1, 0);
                    break;

                case 0xAA:
                case 0xAB: /* STOS */
                    bits = 8 << (opcode & 1);
                    if (!repeating) {
                        wait_cycs(1, 0);
                        if (in_rep != 0)
                            wait_cycs(1, 0);
                    }
                    if (rep_action(bits)) {
                        wait_cycs(1, 0);
                        break;
                    }
                    cpu_data = AX;
                    access(28, bits);
                    stos(bits);
                    if (in_rep == 0) {
                        wait_cycs(3, 0);
                        break;
                    }
                    repeating = 1;
                    clock_end();
                    break;

                case 0xB0:
                case 0xB1:
                case 0xB2:
                case 0xB3: /*MOV cpu_reg,#8*/
                case 0xB4:
                case 0xB5:
                case 0xB6:
                case 0xB7:
                    wait_cycs(1, 0);
                    if (opcode & 0x04)
                        cpu_state.regs[opcode & 0x03].b.h = pfq_fetchb();
                    else
                        cpu_state.regs[opcode & 0x03].b.l = pfq_fetchb();
                    wait_cycs(1, 0);
                    break;

                case 0xB8:
                case 0xB9:
                case 0xBA:
                case 0xBB: /*MOV cpu_reg,#16*/
                case 0xBC:
                case 0xBD:
                case 0xBE:
                case 0xBF:
                    wait_cycs(1, 0);
                    cpu_state.regs[opcode & 0x07].w = pfq_fetchw();
                    wait_cycs(1, 0);
                    break;

                case 0xC0:
                case 0xC1:
                case 0xC2:
                case 0xC3:
                case 0xC8:
                case 0xC9:
                case 0xCA:
                case 0xCB:
                    /* RET */
                    bits = 8 + (opcode & 0x08);
                    if ((opcode & 9) != 1)
                        wait_cycs(1, 0);
                    if (!(opcode & 1)) {
                        cpu_src = pfq_fetchw();
                        wait_cycs(1, 0);
                    }
                    if ((opcode & 9) == 9)
                        wait_cycs(1, 0);
                    pfq_clear();
                    access(26, bits);
                    new_ip = pop();
                    wait_cycs(2, 0);
                    if ((opcode & 8) == 0)
                        new_cs = CS;
                    else {
                        access(42, bits);
                        new_cs = pop();
                        if (opcode & 1)
                            wait_cycs(1, 0);
                    }
                    if (!(opcode & 1)) {
                        SP += cpu_src;
                        wait_cycs(1, 0);
                    }
                    load_cs(new_cs);
                    access(72, bits);
                    set_ip(new_ip);
                    break;

                case 0xC4:
                case 0xC5:
                    /* LsS rw, rmd */
                    do_mod_rm();
                    bits = 16;
                    access(52, bits);
                    read_ea(1, bits);
                    cpu_state.regs[cpu_reg].w = cpu_data;
                    access(57, bits);
                    read_ea2(bits);
                    load_seg(cpu_data, (opcode & 0x01) ? &cpu_state.seg_ds : &cpu_state.seg_es);
                    wait_cycs(1, 0);
                    break;

                case 0xC6:
                case 0xC7:
                    /* MOV rm, imm */
                    bits = 8 << (opcode & 1);
                    do_mod_rm();
                    wait_cycs(1, 0);
                    if (cpu_mod != 3)
                        wait_cycs(2, 0);
                    cpu_data = pfq_fetch();
                    if (cpu_mod == 3)
                        wait_cycs(1, 0);
                    access(16, bits);
                    set_ea(cpu_data);
                    break;

                case 0xCC: /*INT 3*/
                    interrupt(3);
                    break;
                case 0xCD: /*INT*/
                    wait_cycs(1, 0);
                    interrupt(pfq_fetchb());
                    break;
                case 0xCE: /*INTO*/
                    wait_cycs(3, 0);
                    if (cpu_state.flags & V_FLAG) {
                        wait_cycs(2, 0);
                        interrupt(4);
                    }
                    break;

                case 0xCF: /*IRET*/
                    access(43, 8);
                    new_ip = pop();
                    wait_cycs(3, 0);
                    access(44, 8);
                    new_cs = pop();
                    load_cs(new_cs);
                    access(62, 8);
                    set_ip(new_ip);
                    access(45, 8);
                    if (is_nec && cpu_md_write_disable)
                        cpu_state.flags = pop() | 0x8002;
                    else
                        cpu_state.flags = pop() | 0x0002;
                    wait_cycs(5, 0);
                    noint      = 2;
                    nmi_enable = 1;
                    if (is_nec && !(cpu_state.flags & MD_FLAG))
                        sync_to_i8080();
                    break;

                case 0xD0:
                case 0xD1:
                case 0xD2:
                case 0xD3:
                    /* rot rm */
                    bits = 8 << (opcode & 1);
                    do_mod_rm();
                    if (cpu_mod == 3)
                        wait_cycs(1, 0);
                    access(53, bits);
                    cpu_data = get_ea();
                    if ((opcode & 2) == 0) {
                        cpu_src = 1;
                        wait_cycs((cpu_mod != 3) ? 4 : 0, 0);
                    } else {
                        cpu_src = CL;
                        wait_cycs((cpu_mod != 3) ? 9 : 6, 0);
                    }
                    if (is186 && !is_nec)
                        cpu_src &= 0x1F;
                    while (cpu_src != 0) {
                        cpu_dest = cpu_data;
                        oldc     = cpu_state.flags & C_FLAG;
                        switch (rmdat & 0x38) {
                            case 0x00: /* ROL */
                                set_cf(top_bit(cpu_data, bits));
                                cpu_data <<= 1;
                                cpu_data |= ((cpu_state.flags & C_FLAG) ? 1 : 0);
                                set_of_rotate(bits);
                                set_af(0);
                                break;
                            case 0x08: /* ROR */
                                set_cf((cpu_data & 1) != 0);
                                cpu_data >>= 1;
                                if (cpu_state.flags & C_FLAG)
                                    cpu_data |= (!(opcode & 1) ? 0x80 : 0x8000);
                                set_of_rotate(bits);
                                set_af(0);
                                break;
                            case 0x10: /* RCL */
                                set_cf(top_bit(cpu_data, bits));
                                cpu_data = (cpu_data << 1) | (oldc ? 1 : 0);
                                set_of_rotate(bits);
                                set_af(0);
                                break;
                            case 0x18: /* RCR */
                                set_cf((cpu_data & 1) != 0);
                                cpu_data >>= 1;
                                if (oldc)
                                    cpu_data |= (!(opcode & 0x01) ? 0x80 : 0x8000);
                                set_cf((cpu_dest & 1) != 0);
                                set_of_rotate(bits);
                                set_af(0);
                                break;
                            case 0x20: /* SHL */
                                set_cf(top_bit(cpu_data, bits));
                                cpu_data <<= 1;
                                set_of_rotate(bits);
                                set_af((cpu_data & 0x10) != 0);
                                set_pzs(bits);
                                break;
                            case 0x28: /* SHR */
                                set_cf((cpu_data & 1) != 0);
                                cpu_data >>= 1;
                                set_of_rotate(bits);
                                set_af(0);
                                set_pzs(bits);
                                break;
                            case 0x30: /* SETMO - undocumented? */
                                bitwise(bits, 0xffff);
                                set_cf(0);
                                set_of_rotate(bits);
                                set_af(0);
                                set_pzs(bits);
                                break;
                            case 0x38: /* SAR */
                                set_cf((cpu_data & 1) != 0);
                                cpu_data >>= 1;
                                if (!(opcode & 1))
                                    cpu_data |= (cpu_dest & 0x80);
                                else
                                    cpu_data |= (cpu_dest & 0x8000);
                                set_of_rotate(bits);
                                set_af(0);
                                set_pzs(bits);
                                break;
                        }
                        if ((opcode & 2) != 0)
                            wait_cycs(4, 0);
                        --cpu_src;
                    }
                    access(17, bits);
                    set_ea(cpu_data);
                    break;

                case 0xD4: /*AAM*/
                    wait_cycs(1, 0);
#ifdef NO_VARIANT_ON_NEC
                    if (is_nec) {
                        (void) pfq_fetchb();
                        cpu_src = 10;
                    } else
                        cpu_src = pfq_fetchb();
#else
                    cpu_src = pfq_fetchb();
#endif
                    if (x86_div(AL, 0)) {
                        cpu_data = AL;
                        set_pzs(8);
                    }
                    break;
                case 0xD5: /*AAD*/
                    wait_cycs(1, 0);
                    if (is_nec) {
                        (void) pfq_fetchb();
                        mul(10, AH);
                    } else
                        mul(pfq_fetchb(), AH);
                    cpu_dest = AL;
                    cpu_src  = cpu_data;
                    add(8);
                    AL = cpu_data;
                    AH = 0x00;
                    set_pzs(8);
                    break;
                case 0xD6: /*SALC*/
                    if (!is_nec) {
                        wait_cycs(1, 0);
                        AL = (cpu_state.flags & C_FLAG) ? 0xff : 0x00;
                        wait_cycs(1, 0);
                        break;
                    }
                    fallthrough;
                case 0xD7: /*XLATB*/
                    cpu_state.eaaddr = (BX + AL) & 0xffff;
                    access(4, 8);
                    AL = readmemb((ovr_seg ? *ovr_seg : ds) + cpu_state.eaaddr);
                    wait_cycs(1, 0);
                    break;

                case 0xD8:
                case 0xD9:
                case 0xDA:
                case 0xDB:
                case 0xDD:
                case 0xDC:
                case 0xDE:
                case 0xDF:
                    /* esc i, r, rm */
                    do_mod_rm();
                    access(54, 16);
                    tempw = cpu_state.pc;
                    if (!hasfpu)
                        geteaw();
                    else
                        if (fpu_softfloat) {
                            switch (opcode) {
                                case 0xD8:
                                    ops_sf_fpu_8087_d8[(rmdat >> 3) & 0x1f](rmdat);
                                    break;
                                case 0xD9:
                                    ops_sf_fpu_8087_d9[rmdat & 0xff](rmdat);
                                    break;
                                case 0xDA:
                                    ops_sf_fpu_8087_da[rmdat & 0xff](rmdat);
                                    break;
                                case 0xDB:
                                    ops_sf_fpu_8087_db[rmdat & 0xff](rmdat);
                                    break;
                                case 0xDC:
                                    ops_sf_fpu_8087_dc[(rmdat >> 3) & 0x1f](rmdat);
                                    break;
                                case 0xDD:
                                    ops_sf_fpu_8087_dd[rmdat & 0xff](rmdat);
                                    break;
                                case 0xDE:
                                    ops_sf_fpu_8087_de[rmdat & 0xff](rmdat);
                                    break;
                                case 0xDF:
                                    ops_sf_fpu_8087_df[rmdat & 0xff](rmdat);
                                    break;

                                default:
                                    break;
                            }
                        } else {
                            switch (opcode) {
                                case 0xD8:
                                    ops_fpu_8087_d8[(rmdat >> 3) & 0x1f](rmdat);
                                    break;
                                case 0xD9:
                                    ops_fpu_8087_d9[rmdat & 0xff](rmdat);
                                    break;
                                case 0xDA:
                                    ops_fpu_8087_da[rmdat & 0xff](rmdat);
                                    break;
                                case 0xDB:
                                    ops_fpu_8087_db[rmdat & 0xff](rmdat);
                                    break;
                                case 0xDC:
                                    ops_fpu_8087_dc[(rmdat >> 3) & 0x1f](rmdat);
                                    break;
                                case 0xDD:
                                    ops_fpu_8087_dd[rmdat & 0xff](rmdat);
                                    break;
                                case 0xDE:
                                    ops_fpu_8087_de[rmdat & 0xff](rmdat);
                                    break;
                                case 0xDF:
                                    ops_fpu_8087_df[rmdat & 0xff](rmdat);
                                    break;

                                default:
                                    break;
                            }
                        }
                    cpu_state.pc = tempw; /* Do this as the x87 code advances it, which is needed on
                                             the 286+ core, but not here. */
                    wait_cycs(1, 0);
                    if (cpu_mod != 3)
                        wait_cycs(2, 0);
                    break;

                case 0xE0:
                case 0xE1:
                case 0xE2:
                case 0xE3:
                    /* LOOP */
                    wait_cycs(3, 0);
                    cpu_data = pfq_fetchb();
                    if (opcode != 0xe2)
                        wait_cycs(1, 0);
                    if (opcode != 0xe3) {
                        --CX;
                        oldc = (CX != 0);
                        switch (opcode) {
                            case 0xE0:
                                if (cpu_state.flags & Z_FLAG)
                                    oldc = 0;
                                break;
                            case 0xE1:
                                if (!(cpu_state.flags & Z_FLAG))
                                    oldc = 0;
                                break;
                        }
                    } else
                        oldc = (CX == 0);
                    if (oldc)
                        jump_short();
                    break;

                case 0xE4:
                case 0xE5:
                case 0xE6:
                case 0xE7:
                case 0xEC:
                case 0xED:
                case 0xEE:
                case 0xEF:
                    bits = 8 << (opcode & 1);
                    if ((opcode & 0x0e) != 0x0c)
                        wait_cycs(1, 0);
                    if ((opcode & 8) == 0)
                        cpu_data = pfq_fetchb();
                    else
                        cpu_data = DX;
                    cpu_state.eaaddr = cpu_data;
                    if ((opcode & 2) == 0) {
                        access(3, bits);
                        if (opcode & 1)
                            cpu_io(16, 0, cpu_data);
                        else
                            cpu_io(8, 0, cpu_data);
                        wait_cycs(1, 0);
                    } else {
                        if ((opcode & 8) == 0)
                            access(8, bits);
                        else
                            access(9, bits);
                        if (opcode & 1)
                            cpu_io(16, 1, cpu_data);
                        else
                            cpu_io(8, 1, cpu_data);
                    }
                    break;

                case 0xE8: /*CALL rel 16*/
                    wait_cycs(1, 0);
                    cpu_state.oldpc = jump_near();
                    access(34, 8);
                    push((uint16_t *) &(cpu_state.oldpc));
                    break;
                case 0xE9: /*JMP rel 16*/
                    wait_cycs(1, 0);
                    jump_near();
                    break;
                case 0xEA: /*JMP far*/
                    wait_cycs(1, 0);
                    addr = pfq_fetchw();
                    wait_cycs(1, 0);
                    tempw = pfq_fetchw();
                    load_cs(tempw);
                    access(70, 8);
                    pfq_clear();
                    set_ip(addr);
                    break;
                case 0xEB: /*JMP rel*/
                    wait_cycs(1, 0);
                    cpu_data = (int8_t) pfq_fetchb();
                    jump_short();
                    wait_cycs(1, 0);
                    break;

                case 0xF0:
                case 0xF1: /*LOCK - F1 is alias*/
                    in_lock = 1;
                    wait_cycs(1, 0);
                    completed = 0;
                    break;

                case 0xF2: /*REPNE*/
                case 0xF3: /*REPE*/
                    wait_cycs(1, 0);
                    in_rep     = (opcode == 0xf2 ? 1 : 2);
                    completed  = 0;
                    rep_c_flag = 0;
                    break;

                case 0xF4: /*HLT*/
                    if (!repeating) {
                        wait_cycs(1, 0);
                        pfq_clear();
                    }
                    wait_cycs(1, 0);
                    if (irq_pending(is_nec)) {
                        wait_cycs(cycles & 1, 0);
                        check_interrupts(is_nec);
                    } else {
                        repeating = 1;
                        completed = 0;
                        clock_end();
                    }
                    break;
                case 0xF5: /*CMC*/
                    wait_cycs(1, 0);
                    cpu_state.flags ^= C_FLAG;
                    break;

                case 0xF6:
                case 0xF7:
                    bits = 8 << (opcode & 1);
                    do_mod_rm();
                    access(55, bits);
                    cpu_data = get_ea();
                    switch (rmdat & 0x38) {
                        case 0x00:
                        case 0x08:
                            /* TEST */
                            wait_cycs(2, 0);
                            if (cpu_mod != 3)
                                wait_cycs(1, 0);
                            cpu_src = pfq_fetch();
                            wait_cycs(1, 0);
                            test(bits, cpu_data, cpu_src);
                            if (cpu_mod != 3)
                                wait_cycs(1, 0);
                            break;
                        case 0x10: /* NOT */
                        case 0x18: /* NEG */
                            wait_cycs(2, 0);
                            if ((rmdat & 0x38) == 0x10)
                                cpu_data = ~cpu_data;
                            else {
                                cpu_src  = cpu_data;
                                cpu_dest = 0;
                                sub(bits);
                            }
                            access(18, bits);
                            set_ea(cpu_data);
                            break;
                        case 0x20: /* MUL */
                        case 0x28: /* IMUL */
                            old_flags = cpu_state.flags;
                            wait_cycs(1, 0);
                            mul(get_accum(bits), cpu_data);
                            if (opcode & 1) {
                                AX = cpu_data;
                                DX = cpu_dest;
                                set_co_mul(bits, DX != ((AX & 0x8000) == 0 || (rmdat & 0x38) == 0x20 ? 0 : 0xffff));
                                cpu_data = DX;
                            } else {
                                AL = (uint8_t) cpu_data;
                                AH = (uint8_t) cpu_dest;
                                set_co_mul(bits, AH != ((AL & 0x80) == 0 || (rmdat & 0x38) == 0x20 ? 0 : 0xff));
                                if (!is_nec)
                                    cpu_data = AH;
                            }
                            set_sf(bits);
                            set_pf();
                            if (cpu_mod != 3)
                                wait_cycs(1, 0);
                            /* NOTE: When implementing the V20, care should be taken to not change
                                     the zero flag. */
                            if (is_nec)
                                cpu_state.flags = (cpu_state.flags & ~Z_FLAG) | (old_flags & Z_FLAG);
                            break;
                        case 0x30: /* DIV */
                        case 0x38: /* IDIV */
                            if (cpu_mod != 3)
                                wait_cycs(1, 0);
                            cpu_src = cpu_data;
                            if (x86_div(AL, AH))
                                wait_cycs(1, 0);
                            break;
                    }
                    break;

                case 0xF8:
                case 0xF9:
                    /* CLCSTC */
                    wait_cycs(1, 0);
                    set_cf(opcode & 1);
                    break;
                case 0xFA:
                case 0xFB:
                    /* CLISTI */
                    wait_cycs(1, 0);
                    set_if(opcode & 1);
                    break;
                case 0xFC:
                case 0xFD:
                    /* CLDSTD */
                    wait_cycs(1, 0);
                    set_df(opcode & 1);
                    break;

                case 0xFE:
                case 0xFF:
                    /* misc */
                    bits = 8 << (opcode & 1);
                    do_mod_rm();
                    access(56, bits);
                    read_ea(((rmdat & 0x38) == 0x18) || ((rmdat & 0x38) == 0x28), bits);
                    switch (rmdat & 0x38) {
                        case 0x00: /* INC rm */
                        case 0x08: /* DEC rm */
                            cpu_dest = cpu_data;
                            cpu_src  = 1;
                            if ((rmdat & 0x38) == 0x00) {
                                cpu_data = cpu_dest + cpu_src;
                                set_of_add(bits);
                            } else {
                                cpu_data = cpu_dest - cpu_src;
                                set_of_sub(bits);
                            }
                            do_af();
                            set_pzs(bits);
                            wait_cycs(2, 0);
                            access(19, bits);
                            set_ea(cpu_data);
                            break;
                        case 0x10: /* CALL rm */
                            cpu_data_opff_rm();
                            access(63, bits);
                            wait_cycs(1, 0);
                            pfq_clear();
                            wait_cycs(4, 0);
                            if (cpu_mod != 3)
                                wait_cycs(1, 0);
                            wait_cycs(1, 0); /* Wait. */
                            cpu_state.oldpc = cpu_state.pc;
                            set_ip(cpu_data);
                            wait_cycs(2, 0);
                            access(35, bits);
                            push((uint16_t *) &(cpu_state.oldpc));
                            break;
                        case 0x18: /* CALL rmd */
                            new_ip = cpu_data;
                            access(58, bits);
                            read_ea2(bits);
                            if (!(opcode & 1))
                                cpu_data |= 0xff00;
                            new_cs = cpu_data;
                            access(36, bits);
                            push(&(CS));
                            access(64, bits);
                            wait_cycs(4, 0);
                            cpu_state.oldpc = cpu_state.pc;
                            load_cs(new_cs);
                            set_ip(new_ip);
                            access(37, bits);
                            push((uint16_t *) &(cpu_state.oldpc));
                            break;
                        case 0x20: /* JMP rm */
                            cpu_data_opff_rm();
                            access(65, bits);
                            set_ip(cpu_data);
                            break;
                        case 0x28: /* JMP rmd */
                            new_ip = cpu_data;
                            access(59, bits);
                            read_ea2(bits);
                            if (!(opcode & 1))
                                cpu_data |= 0xff00;
                            new_cs = cpu_data;
                            load_cs(new_cs);
                            access(66, bits);
                            set_ip(new_ip);
                            break;
                        case 0x30: /* PUSH rm */
                        case 0x38:
                            if (cpu_mod != 3)
                                wait_cycs(1, 0);
                            access(38, bits);
                            push((uint16_t *) &(cpu_data));
                            break;
                    }
                    break;

                default:
                    x808x_log("Illegal opcode: %02X\n", opcode);
                    pfq_fetchb();
                    wait_cycs(8, 0);
                    break;
            }
        }
exec_completed:
        if (completed) {
            repeating  = 0;
            ovr_seg    = NULL;
            in_rep     = 0;
            rep_c_flag = 0;
            if (in_lock)
                clear_lock = 1;
            clock_end();
            check_interrupts(0);

            if (noint)
                noint = 0;

            cpu_alu_op = 0;
        }

#ifdef USE_GDBSTUB
        if (gdbstub_instruction())
            return;
#endif
    }
}
