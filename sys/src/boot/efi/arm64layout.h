#ifndef PLAN2001_ARM64_LAYOUT_H
#define PLAN2001_ARM64_LAYOUT_H
#include "image.h"
/* efiplatform=qemu-virt: frozen QEMU a.out or linked ELF64.
 * efiplatform=rpi4: fixed 9front ARM64 headed a.out or linked ELF64 ET_EXEC, plus mandatory
 * efibootmap=physical-base,length and efiscratch=physical-base,length.
 * Pi4's kernel must map BootInfo in that direct window at entry; all
 * early writes must be within PT_LOAD allocations or efiscratch (0,0
 * means no external scratch). No relocation or firmware DTB is inferred.
 * Pi4 uses 64 KiB-aligned allocations; ELF p_paddr and p_vaddr must agree
 * with its KZERO. The native a.out entry is fixed by bcm64/mem.h and its
 * pre-text scratch is explicit. Raw -H6 images have no such contract.
 */
typedef struct ArmLayout ArmLayout;
enum { ArmMaxReserved=128, ArmMaxDynamic=8, ArmMaxAllocRanges=8 };
typedef struct ArmDynamic ArmDynamic;
struct ArmDynamic {
	uvlong size, align;
	int nranges;
	uvlong rangelo[ArmMaxAllocRanges], rangelen[ArmMaxAllocRanges];
};
struct ArmLayout {
	int platform;
	uvlong kzero, maplo, maplen, scratchlo, scratchlen, fdtlo, fdtlen, align;
	int nreserved;
	uvlong reservedlo[ArmMaxReserved], reservedlen[ArmMaxReserved];
};
enum { ArmQemu=1, ArmPi4 };
char *armplatform(ArmLayout*, char*, int, uvlong, uvlong, uvlong, uvlong);
char *armimage(ArmLayout*, BootImage*);
char *armfdtvalidate(void*, uvlong);
char *armfdtreservations(void*, uvlong, uvlong*, uvlong*, int, int*, ArmDynamic*, int, int*);
char *armmapvalid(void*, uvlong, uvlong);
int armpresent(void*, uvlong, uvlong, uvlong, uvlong);
char *armmemory(ArmLayout*, BootImage*, void*, uvlong, uvlong);
int armblob(ArmLayout*, BootImage*, void*, uvlong, uvlong, uvlong, uvlong*);
#endif
