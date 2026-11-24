#include <u.h>
#include "fns.h"
#include "efi.h"
#include "mem.h"

/* 9front-11952 ARM64 QEMU virt: KZERO + physical address, X0=BootInfo.
 * The kernel handles EL1/EL2 entry and builds its own translation tables.
 */

void jump(void *pc, void *bootinfo);

enum {
	RamBase	= 0x40000000,		/* QEMU virt */
	KernelLow = RamBase + 16*1024*1024,	/* blob stays above image + tables */
	KernelMap = 0x80000000ULL,	/* the kernel's boot map covers 0..2 GiB */
};

/* the ISA into the BootInfo; there is no TSC (its tscfreq stays 0) */
void
archconf(BootInfo *bi)
{
	bi->arch = BootArchArm64;
}

/* the kernel's physical entry from its ELF entry: linked at KZERO + pa */
uvlong
archentry(uvlong entry)
{
	return entry - 0xffffffff80000000ULL;
}

/*
 * where 7l starts the kernel's data after its text: the next 64 KB (its
 * default -R; the kernel's mkfile links without one), not the next page.
 * The ELF path does not use this.
 */
ulong
archdataround(void)
{
	return 0x10000;
}

/*
 * May the BootInfo blob lie at [pa, pa+len)?  Not in the first 16 MB of
 * RAM (the kernel's image and boot page tables), and within the physical
 * range the kernel's boot map covers.
 */
int
archblobok(uvlong pa, uvlong len)
{
	return pa >= KernelLow && pa < KernelMap && len <= KernelMap - pa;
}

/* ELF-ytimen e_machine (sub.c bootelf()): EM_AARCH64 */
int
archelfmachine(void)
{
	return 183;
}

/* the kernel's entry must be in RAM the boot map covers */
int
archentryok(uvlong entry)
{
	uvlong pa;

	if(entry < 0xffffffff80000000ULL)
		return 0;
	pa = entry - 0xffffffff80000000ULL;
	return pa >= RamBase && pa < KernelMap;
}

/*
 * The loader wrote the image through the firmware's caches.  Clean each
 * loaded range to the point of coherency before the kernel entry turns the
 * MMU and caches off; archcachesync() then makes the instructions visible.
 * aa64.s implements both.
 */
void
archcacheclean(uvlong pa, uvlong len)
{
	extern void cacheclean(uvlong, uvlong);

	cacheclean(pa, len);
}

void
archcachesync(void)
{
	extern void cachesync(void);

	cachesync();
}

/* nil if the kernel can be entered from here: it drops to EL1 itself */
char*
archcheck(void)
{
	return nil;
}

/*
 * Enter the kernel: X0 = BootInfo physical address (aa64.s).  jump()
 * cleans firmware cache writes. The frozen kernel entry handles EL1/EL2,
 * disables the MMU, clears BSS and installs its own mappings.
 */
void
archjump(void *entry, void *bootinfo)
{
	jump(entry, bootinfo);
}
