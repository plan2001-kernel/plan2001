#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"

uintptr bootinfopa;

/* mmu0init maps QEMU RAM [0x40000000, 0x80000000) before validation. */
void*
bootearlymap(uvlong pa, uvlong size)
{
	if((pa & (BY2PG-1)) != 0 || pa < 0x40000000ULL
	|| pa >= 0x80000000ULL || size > 0x80000000ULL-pa)
		return nil;
	return (void*)(uintptr)(KZERO + pa);
}

void
halt(void)
{
	splhi();
	for(;;)
		;
}

/* Reboot configuration is not written back into firmware memory. */
void
writeconf(void)
{
}
