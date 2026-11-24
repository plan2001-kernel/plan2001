#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"

/* QEMU virt: the initial direct map covers RAM below 2 GiB. */
#define INITMAP	((uintptr)-KZERO)

/*
 * Create initial identity map in top-level page table
 * (L1BOT) for TTBR0. This page table is only used until
 * mmu1init() loads m->mmutop.
 */
void
mmuidmap(uintptr *l1bot)
{
	uintptr pa, pe, attr;

	/* VDRAM */
	attr = PTEWRITE | PTEAF | PTEKERNEL | PTEUXN | PTESH(SHARE_INNER);
	pe = -KZERO;
	for(pa = VDRAM - KZERO; pa < pe; pa += PGLSZ(PTLEVELS-1))
		l1bot[PTLX(pa, PTLEVELS-1)] = pa | PTEVALID | PTEBLOCK | attr;
}

/*
 * Create initial shared kernel page table (L1) for TTBR1.
 * This page table coveres the INITMAP and VIRTIO,
 * and later we fill the ram mappings in meminit().
 */
void
mmu0init(uintptr *l1)
{
	uintptr va, pa, pe, attr;

	/* DRAM - INITMAP */
	attr = PTEWRITE | PTEAF | PTEKERNEL | PTEUXN | PTESH(SHARE_INNER);
	pe = INITMAP;
	for(pa = VDRAM - KZERO, va = VDRAM; pa < pe; pa += PGLSZ(1), va += PGLSZ(1))
		l1[PTL1X(va, 1)] = pa | PTEVALID | PTEBLOCK | attr;

	/* VIRTIO */
	attr = PTEWRITE | PTEAF | PTEKERNEL | PTEUXN | PTEPXN | PTESH(SHARE_OUTER) | PTEDEVICE;
	pe = PHYSIOEND;
	for(pa = PHYSIO, va = VIRTIO; pa < pe; pa += PGLSZ(1), va += PGLSZ(1)){
		if(((pa|va) & PGLSZ(1)-1) != 0){
			l1[PTL1X(va, 1)] = (uintptr)l1 | PTEVALID | PTETABLE;
			for(; pa < pe && ((va|pa) & PGLSZ(1)-1) != 0; pa += PGLSZ(0), va += PGLSZ(0)){
				assert(l1[PTLX(va, 0)] == 0);
				l1[PTLX(va, 0)] = pa | PTEVALID | PTEPAGE | attr;
			}
			break;
		}
		l1[PTL1X(va, 1)] = pa | PTEVALID | PTEBLOCK | attr;
	}

	if(PTLEVELS > 2)
	for(va = KSEG0; va != 0; va += PGLSZ(2))
		l1[PTL1X(va, 2)] = (uintptr)&l1[L1TABLEX(va, 1)] | PTEVALID | PTETABLE;

	if(PTLEVELS > 3)
	for(va = KSEG0; va != 0; va += PGLSZ(3))
		l1[PTL1X(va, 3)] = (uintptr)&l1[L1TABLEX(va, 2)] | PTEVALID | PTETABLE;
}

/* Only conventional RAM is allocatable: firmware, loader, BootInfo and
 * root-image allocations remain reserved. Keep the largest contiguous bank
 * in the initial direct map; no unverified FDT maxmem or fixed RAM size. */
void
meminit(void)
{
	BootMem *bm;
	uintptr b, e, size;
	int i;

	size = 0;
	for(i = 0; i < bootinfo->mmapcount; i++){
		bm = bootmem(i);
		if(bm->type != BootMemConventional)
			continue;
		b = PGROUND(bm->base);
		e = (bm->base + bm->len) & -BY2PG;
		if(b < PGROUND((uintptr)end - KZERO))
			b = PGROUND((uintptr)end - KZERO);
		if(e > (uintptr)-KZERO)
			e = (uintptr)-KZERO;
		if(e > b && e-b > size){
			conf.mem[0].base = b;
			conf.mem[0].limit = e;
			size = e-b;
		}
	}
	if(size < 16*MiB)
		panic("BootInfo: insufficient conventional QEMU RAM");
	conf.mem[0].npage = size/BY2PG;
}
