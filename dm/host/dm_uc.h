// Registers of a signal handler's machine context: Darwin keeps them in uc_mcontext->__ss / __ns / __es, Linux in uc_mcontext.regs / pc / sp
// and in the records of uc_mcontext.__reserved (FPSIMD, ESR)
#pragma once
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <sys/ucontext.h>
#ifdef __APPLE__
#define UC_X(uc, n) ((uc)->uc_mcontext->__ss.__x[n])   // x0..x28
#define UC_FP(uc) ((uc)->uc_mcontext->__ss.__fp)
#define UC_LR(uc) ((uc)->uc_mcontext->__ss.__lr)
#define UC_SP(uc) ((uc)->uc_mcontext->__ss.__sp)
#define UC_PC(uc) ((uc)->uc_mcontext->__ss.__pc)
#define UC_PSTATE(uc) ((uc)->uc_mcontext->__ss.__cpsr)
static inline uint64_t uc_esr(ucontext_t *uc) { return uc->uc_mcontext->__es.__esr; }
// v0..v31 (16 bytes each), fpcr, fpsr; 0 when the context has none
static inline int uc_fpsimd(ucontext_t *uc, void *v, uint32_t *fpcr, uint32_t *fpsr) {
    memcpy(v, uc->uc_mcontext->__ns.__v, 32 * 16); *fpcr = uc->uc_mcontext->__ns.__fpcr; *fpsr = uc->uc_mcontext->__ns.__fpsr; return 1;
}
#else
#define UC_X(uc, n) ((uc)->uc_mcontext.regs[n])
#define UC_FP(uc) ((uc)->uc_mcontext.regs[29])
#define UC_LR(uc) ((uc)->uc_mcontext.regs[30])
#define UC_SP(uc) ((uc)->uc_mcontext.sp)
#define UC_PC(uc) ((uc)->uc_mcontext.pc)
#define UC_PSTATE(uc) ((uc)->uc_mcontext.pstate)
// The records in __reserved: {u32 magic, u32 size} headers, ended by magic 0
static inline const uint8_t *uc_record(ucontext_t *uc, uint32_t magic) {
    const uint8_t *p = (const uint8_t *)uc->uc_mcontext.__reserved, *e = p + sizeof uc->uc_mcontext.__reserved;
    while (p + 8 <= e) {
        uint32_t m, sz; memcpy(&m, p, 4); memcpy(&sz, p + 4, 4);
        if (!m || sz < 8) return NULL;
        if (m == magic) return p;
        p += sz;
    }
    return NULL;
}
static inline uint64_t uc_esr(ucontext_t *uc) { const uint8_t *r = uc_record(uc, 0x45535201); uint64_t v = 0; if (r) memcpy(&v, r + 8, 8); return v; }   // ESR_MAGIC
static inline int uc_fpsimd(ucontext_t *uc, void *v, uint32_t *fpcr, uint32_t *fpsr) {   // FPSIMD_MAGIC: {head, fpsr, fpcr, vregs[32]}
    const uint8_t *r = uc_record(uc, 0x46508001);
    if (!r) return 0;
    memcpy(fpsr, r + 8, 4); memcpy(fpcr, r + 12, 4); memcpy(v, r + 16, 32 * 16); return 1;
}
#endif
