#ifndef PLAN2001_ARM64_LAYOUT_H
#define PLAN2001_ARM64_LAYOUT_H
#include "image.h"
/* efiplatform=qemu-virt: frozen QEMU a.out or linked ELF64.
 * efiplatform=rpi4: linked ELF64 ET_EXEC only, plus mandatory
 * efibootmap=physical-base,length and efiscratch=physical-base,length.
 * Pi4's kernel must map BootInfo in that direct window at entry; all
 * early writes must be within PT_LOAD allocations or efiscratch (0,0
 * means no external scratch). No relocation or firmware DTB is inferred.
 * Pi4 uses 64 KiB-aligned allocations; ELF p_paddr and p_vaddr must agree
 * with its KZERO. Raw -H6 images have no such contract and are rejected.
 */
typedef struct ArmLayout ArmLayout;
struct ArmLayout {
	int platform;
	uvlong kzero, maplo, maplen, scratchlo, scratchlen, align;
};
enum { ArmQemu=1, ArmPi4 };
char *armplatform(ArmLayout*, char*, int, uvlong, uvlong, uvlong, uvlong);
char *armimage(ArmLayout*, BootImage*);
char *armmapvalid(void*, uvlong, uvlong);
char *armmemory(ArmLayout*, BootImage*, void*, uvlong, uvlong);
int armblob(ArmLayout*, BootImage*, void*, uvlong, uvlong, uvlong, uvlong*);
#endif
