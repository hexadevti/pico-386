#ifndef FPU_H
#define FPU_H

#include <stdbool.h>
#include <stdint.h>

typedef struct FPU FPU;

FPU *fpu_new();
void fpu_delete(FPU *fpu);
bool fpu_exec1(FPU *fpu, void *cpu, int op, int group, unsigned int i);
bool fpu_exec2(FPU *fpu, void *cpu, bool opsz16, int op, int group, int seg, uword addr);

#ifdef TINY386_PICO
struct SnapIO;
void fpu_snap(FPU *fpu, struct SnapIO *io);   /* pico-386: snap.h */
#endif

#endif /* FPU_H */
