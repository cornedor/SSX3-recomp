/*
 * MMIO trap for the POSIX host: the counterpart of the Windows VEH path.
 *
 * Device registers (APU, OHCI) are unmapped guest pages. A guest load/store
 * faults; the SIGSEGV handler decodes the *host* instruction, runs the device
 * model, writes the result back and steps over it. Unknown encodings are
 * declined (never guessed at) so the crash reporter names the opcode.
 *
 * aarch64: the GPR load/store forms a compiler emits for a single
 * volatile-ish access (LDP/STP, SIMD, exclusives are not). x86-64: see below.
 */
#if !defined(_WIN32)
#define _GNU_SOURCE
#include <signal.h>
#include <ucontext.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "mmio_posix.h"

#if defined(__aarch64__)

typedef struct {
    int      is_write;
    int      size;       /* bytes */
    int      rt;         /* 0..31 (31 = zero register) */
    int      sign_ext;   /* 0 none, 32 -> W destination, 64 -> X destination */
    int      rn;         /* base register, for writeback */
    int      wb_mode;    /* 0 none, 1 pre-index, 2 post-index */
    int64_t  wb_imm;
} a64_access;

static int a64_decode(uint32_t insn, a64_access *a)
{
    uint32_t size = insn >> 30, opc = (insn >> 22) & 3;

    /* Load/store register: bits 29:27 = 111, V (26) = 0. */
    if (((insn >> 27) & 7) != 7 || ((insn >> 26) & 1))
        return 0;

    switch ((insn >> 24) & 3) {
    case 1:                                  /* unsigned offset */
        break;
    case 0:
        if ((insn >> 21) & 1) {
            if (((insn >> 10) & 3) != 2)     /* register offset only */
                return 0;
        } else {
            switch ((insn >> 10) & 3) {      /* imm9 forms */
            case 0: break;                   /* LDUR/STUR */
            case 1: a->wb_mode = 2; break;   /* post-index */
            case 3: a->wb_mode = 1; break;   /* pre-index */
            default: return 0;               /* unprivileged */
            }
            a->wb_imm = ((int64_t)((insn >> 12) & 0x1FF) << 55) >> 55;
        }
        break;
    default:
        return 0;
    }

    a->size = 1 << size;
    a->rt = insn & 31;
    a->rn = (insn >> 5) & 31;
    if (a->rn == 31)
        return 0;                            /* SP-based: not device access */

    switch (opc) {
    case 0: a->is_write = 1; break;
    case 1: a->is_write = 0; break;
    case 2:                                  /* LDRSx -> X */
        if (size == 3) return 0;             /* PRFM */
        a->sign_ext = 64; break;
    case 3:                                  /* LDRSx -> W */
        if (size >= 2) return 0;
        a->sign_ext = 32; break;
    }
    return 1;
}

int mmio_posix_step(void *uc_, uintptr_t fault, mmio_read_fn rd, mmio_write_fn wr,
                    uint32_t *insn_out)
{
    ucontext_t *uc = (ucontext_t *)uc_;
    uint32_t insn = *(const uint32_t *)uc->uc_mcontext.pc;
    a64_access a = {0};

    if (insn_out)
        *insn_out = insn;
    if (!a64_decode(insn, &a))
        return 0;

    if (a.is_write) {
        uint64_t v = a.rt == 31 ? 0 : uc->uc_mcontext.regs[a.rt];
        if (a.size < 8)
            v &= (1ULL << (a.size * 8)) - 1;
        wr(fault, v, a.size);
    } else {
        uint64_t v = rd(fault, a.size);
        if (a.size < 8)
            v &= (1ULL << (a.size * 8)) - 1;
        if (a.sign_ext) {
            int sh = 64 - a.size * 8;
            v = (uint64_t)(((int64_t)(v << sh)) >> sh);
            if (a.sign_ext == 32)
                v &= 0xFFFFFFFFULL;
        }
        if (a.rt != 31)
            uc->uc_mcontext.regs[a.rt] = v;
    }

    if (a.wb_mode == 1)
        uc->uc_mcontext.regs[a.rn] = fault;
    else if (a.wb_mode == 2)
        uc->uc_mcontext.regs[a.rn] = fault + a.wb_imm;

    uc->uc_mcontext.pc += 4;
    return 1;
}

#elif defined(__x86_64__)

/* x86-64: the forms a compiler emits for one guest access through a pointer --
 * MOV both ways (incl. immediates), MOVZX/MOVSX, and the read-modify-write /
 * compare forms (ALU r/m,r and r,r/m, group 1 immediates, TEST, F6/F7 TEST imm,
 * INC/DEC/NOT/NEG). SSE, string ops and LOCK'ed forms are declined. */

static uint64_t *x64_reg(ucontext_t *uc, int r)
{
    static const int map[16] = {
        REG_RAX, REG_RCX, REG_RDX, REG_RBX, REG_RSP, REG_RBP, REG_RSI, REG_RDI,
        REG_R8, REG_R9, REG_R10, REG_R11, REG_R12, REG_R13, REG_R14, REG_R15 };
    return (uint64_t *)&uc->uc_mcontext.gregs[map[r & 15]];
}

/* Register operand of `size` bytes. Without REX, 8-bit regs 4..7 are AH..BH. */
static uint64_t x64_get(ucontext_t *uc, int r, int size, int has_rex)
{
    if (size == 1 && !has_rex && r >= 4 && r < 8)
        return (*x64_reg(uc, r - 4) >> 8) & 0xFF;
    return *x64_reg(uc, r);
}

static void x64_set(ucontext_t *uc, int r, int size, int has_rex, uint64_t v)
{
    uint64_t *d;
    if (size == 1 && !has_rex && r >= 4 && r < 8) {
        d = x64_reg(uc, r - 4);
        *d = (*d & ~0xFF00ULL) | ((v & 0xFF) << 8);
        return;
    }
    d = x64_reg(uc, r);
    if (size == 1)      *d = (*d & ~0xFFULL) | (v & 0xFF);
    else if (size == 2) *d = (*d & ~0xFFFFULL) | (v & 0xFFFF);
    else if (size == 4) *d = v & 0xFFFFFFFFULL;
    else                *d = v;
}

static int x64_modrm_len(const uint8_t *m)
{
    int mod = m[0] >> 6, rm = m[0] & 7, len = 1;
    if (mod == 3) return -1;                         /* register: no access */
    if (rm == 4) {
        len++;
        if (mod == 0 && (m[1] & 7) == 5) len += 4;   /* SIB, no base: disp32 */
    } else if (mod == 0 && rm == 5) {
        len += 4;                                    /* RIP-relative */
    }
    if (mod == 1) len += 1;
    else if (mod == 2) len += 4;
    return len;
}

/* No read accessor (the AC'97 write trap): the page is plain memory then. */
static uint64_t x64_raw(uintptr_t addr, int size)
{
    uint64_t v = 0;
    memcpy(&v, (const void *)addr, (size_t)size);
    return v;
}

static uint64_t x64_mask(int size)
{
    return size >= 8 ? ~0ULL : (1ULL << (size * 8)) - 1;
}

/* Flags of an ALU result: CF, PF, ZF, SF, OF. */
static void x64_flags(ucontext_t *uc, int op, uint64_t a, uint64_t b,
                      uint64_t r, int size)
{
    uint64_t m = x64_mask(size), sb = 1ULL << (size * 8 - 1);
    uint64_t f = (uint64_t)uc->uc_mcontext.gregs[REG_EFL];
    uint8_t lo;
    int cf = 0, of = 0, par = 1;

    a &= m; b &= m; r &= m;
    switch (op) {
    case 0: cf = r < a; of = !!(~(a ^ b) & (a ^ r) & sb); break;        /* add */
    case 5: case 7: cf = a < b; of = !!((a ^ b) & (a ^ r) & sb); break; /* sub/cmp */
    default: break;                                  /* logic: CF = OF = 0 */
    }
    for (lo = (uint8_t)r; lo; lo &= (uint8_t)(lo - 1))
        par ^= 1;
    f &= ~(uint64_t)(0x001 | 0x004 | 0x010 | 0x040 | 0x080 | 0x800);
    if (cf) f |= 0x001;
    if (par) f |= 0x004;
    if (r == 0) f |= 0x040;
    if (r & sb) f |= 0x080;
    if (of) f |= 0x800;
    uc->uc_mcontext.gregs[REG_EFL] = (greg_t)f;
}

/* ALU op index as in the 0x00..0x3F / group-1 encoding: add or adc sbb and sub
 * xor cmp. adc/sbb are declined (carry-in not modelled). */
static int x64_alu(int op, uint64_t a, uint64_t b, uint64_t *r)
{
    switch (op) {
    case 0: *r = a + b; return 1;
    case 1: *r = a | b; return 1;
    case 4: *r = a & b; return 1;
    case 5: case 7: *r = a - b; return 1;
    case 6: *r = a ^ b; return 1;
    default: return 0;
    }
}

int mmio_posix_step(void *uc_, uintptr_t fault, mmio_read_fn rd, mmio_write_fn wr,
                    uint32_t *insn_out)
{
    ucontext_t *uc = (ucontext_t *)uc_;
    const uint8_t *ip = (const uint8_t *)uc->uc_mcontext.gregs[REG_RIP];
    int pre = 0, has66 = 0, rex = 0, size, reg, ml, n;
    const uint8_t *op;
    uint64_t a, b, r;

    if (insn_out)
        *insn_out = (uint32_t)ip[0] << 24 | (uint32_t)ip[1] << 16
                  | (uint32_t)ip[2] << 8 | ip[3];
    for (;;) {
        uint8_t c = ip[pre];
        if (c == 0x66) { has66 = 1; pre++; }
        else if (c == 0x2E || c == 0x3E || c == 0x26 || c == 0x36) pre++;
        else break;
    }
    if ((ip[pre] & 0xF0) == 0x40) rex = ip[pre++];
    op = ip + pre;
    size = (rex & 8) ? 8 : has66 ? 2 : 4;

#define RD(sz)     (rd ? rd(fault, (sz)) : x64_raw(fault, (sz)))
#define REGF(m)    ((((m) >> 3) & 7) | ((rex & 4) ? 8 : 0))
#define DONE(len)  do { uc->uc_mcontext.gregs[REG_RIP] += pre + (len); return 1; } while (0)

    /* ALU r/m,r (00,08,..38 +0/+1) and r,r/m (+2/+3). */
    if (op[0] < 0x40 && (op[0] & 7) < 4) {
        int alu = op[0] >> 3, sz = (op[0] & 1) ? size : 1, dir = op[0] & 2;
        if ((ml = x64_modrm_len(op + 1)) < 0) return 0;
        reg = REGF(op[1]);
        a = RD(sz) & x64_mask(sz);
        b = x64_get(uc, reg, sz, rex) & x64_mask(sz);
        if (dir) { uint64_t t = a; a = b; b = t; }   /* reg op mem */
        if (!x64_alu(alu, a, b, &r)) return 0;
        x64_flags(uc, alu, a, b, r, sz);
        if (alu != 7) {
            if (dir) x64_set(uc, reg, sz, rex, r);
            else     wr(fault, r & x64_mask(sz), sz);
        }
        DONE(1 + ml);
    }

    switch (op[0]) {
    case 0x80: case 0x81: case 0x83: {                 /* group 1 r/m, imm */
        int sz = op[0] == 0x80 ? 1 : size, alu = (op[1] >> 3) & 7, il;
        if ((ml = x64_modrm_len(op + 1)) < 0) return 0;
        if (op[0] == 0x81) { il = sz == 2 ? 2 : 4;
            b = sz == 2 ? *(const uint16_t *)(op + 1 + ml)
                        : (uint64_t)(int64_t)*(const int32_t *)(op + 1 + ml); }
        else { il = 1; b = (uint64_t)(int64_t)(int8_t)op[1 + ml]; }
        a = RD(sz) & x64_mask(sz);
        b &= x64_mask(sz);
        if (!x64_alu(alu, a, b, &r)) return 0;
        x64_flags(uc, alu, a, b, r, sz);
        if (alu != 7) wr(fault, r & x64_mask(sz), sz);
        DONE(1 + ml + il);
    }
    case 0x84: case 0x85: {                            /* TEST r/m, r */
        int sz = op[0] == 0x84 ? 1 : size;
        if ((ml = x64_modrm_len(op + 1)) < 0) return 0;
        a = RD(sz);
        b = x64_get(uc, REGF(op[1]), sz, rex);
        x64_flags(uc, 4, a, b, a & b, sz);
        DONE(1 + ml);
    }
    case 0x88: case 0x89: {                            /* MOV r/m, r */
        int sz = op[0] == 0x88 ? 1 : size;
        if ((ml = x64_modrm_len(op + 1)) < 0) return 0;
        wr(fault, x64_get(uc, REGF(op[1]), sz, rex) & x64_mask(sz), sz);
        DONE(1 + ml);
    }
    case 0x8A: case 0x8B: {                            /* MOV r, r/m */
        int sz = op[0] == 0x8A ? 1 : size;
        if ((ml = x64_modrm_len(op + 1)) < 0) return 0;
        x64_set(uc, REGF(op[1]), sz, rex, RD(sz));
        DONE(1 + ml);
    }
    case 0xC6: case 0xC7: {                            /* MOV r/m, imm */
        int sz = op[0] == 0xC6 ? 1 : size, il;
        if (((op[1] >> 3) & 7) != 0 || (ml = x64_modrm_len(op + 1)) < 0) return 0;
        if (sz == 1)      { il = 1; b = op[1 + ml]; }
        else if (sz == 2) { il = 2; b = *(const uint16_t *)(op + 1 + ml); }
        else { il = 4; b = (uint64_t)(int64_t)*(const int32_t *)(op + 1 + ml); }
        wr(fault, b & x64_mask(sz), sz);
        DONE(1 + ml + il);
    }
    case 0xF6: case 0xF7: {                            /* TEST imm / NOT / NEG */
        int sz = op[0] == 0xF6 ? 1 : size, sub = (op[1] >> 3) & 7, il = 0;
        if ((ml = x64_modrm_len(op + 1)) < 0) return 0;
        a = RD(sz) & x64_mask(sz);
        if (sub == 0) {
            il = sz == 1 ? 1 : sz == 2 ? 2 : 4;
            b = sz == 1 ? op[1 + ml] : sz == 2 ? *(const uint16_t *)(op + 1 + ml)
                : (uint64_t)(int64_t)*(const int32_t *)(op + 1 + ml);
            x64_flags(uc, 4, a, b, a & b, sz);
        } else if (sub == 2) {
            wr(fault, ~a & x64_mask(sz), sz);
        } else if (sub == 3) {
            x64_flags(uc, 5, 0, a, 0 - a, sz);
            wr(fault, (0 - a) & x64_mask(sz), sz);
        } else {
            return 0;
        }
        DONE(1 + ml + il);
    }
    case 0xFE: case 0xFF: {                            /* INC / DEC */
        int sz = op[0] == 0xFE ? 1 : size, sub = (op[1] >> 3) & 7;
        uint64_t cf;
        if (sub > 1 || (ml = x64_modrm_len(op + 1)) < 0) return 0;
        a = RD(sz) & x64_mask(sz);
        cf = (uint64_t)uc->uc_mcontext.gregs[REG_EFL] & 1;    /* INC/DEC keep CF */
        r = sub ? a - 1 : a + 1;
        x64_flags(uc, sub ? 5 : 0, a, 1, r, sz);
        uc->uc_mcontext.gregs[REG_EFL] = (greg_t)(((uint64_t)uc->uc_mcontext.gregs[REG_EFL] & ~1ULL) | cf);
        wr(fault, r & x64_mask(sz), sz);
        DONE(1 + ml);
    }
    case 0x0F:
        switch (op[1]) {
        case 0xB6: case 0xB7: case 0xBE: case 0xBF: {  /* MOVZX / MOVSX */
            int s = (op[1] & 1) ? 2 : 1, sh;
            if ((ml = x64_modrm_len(op + 2)) < 0) return 0;
            a = RD(s) & x64_mask(s);
            if (op[1] >= 0xBE) { sh = 64 - s * 8; a = (uint64_t)((int64_t)(a << sh) >> sh); }
            x64_set(uc, REGF(op[2]), size, rex, a);
            DONE(2 + ml);
        }
        default:
            return 0;
        }
    default:
        return 0;
    }
#undef RD
#undef REGF
#undef DONE
}

#else

int mmio_posix_step(void *uc, uintptr_t fault, mmio_read_fn rd, mmio_write_fn wr,
                    uint32_t *insn_out)
{
    (void)uc; (void)fault; (void)rd; (void)wr; (void)insn_out;
    return 0;                                /* other hosts: not done */
}

#endif
#endif /* !_WIN32 */
