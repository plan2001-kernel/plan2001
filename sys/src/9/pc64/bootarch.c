#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"

/*
 * port/bootinfo.c:n AMD64-puoli.  UEFI-loader (sys/src/boot/efi) hyppää
 * ytimen 64-bittiseen sisääntuloon _efi64 (l.s) BootInfo v1 -blobin
 * fyysinen osoite RDI:ssä; _efi64 tallettaa sen bootinfopa:han.  Blobi
 * voi olla missä tahansa fyysisessä muistissa, mutta _efi64:n
 * käynnistystaulut kartoittavat vain ytimen itsensä.  bootearlymap()
 * kartoittaa blobin omaan virtuaali-ikkunaansa BOOTMAPVA (mem.h) kahdella
 * omalla sivutaulusivulla (PD ja PT KZERO:n PDP:n alla): rampage() ei
 * ole vielä käytettävissä, koska se tarvitsee muistikartan, joka on
 * blobissa.  BOOTMAPSIZE (yhden PT:n kattama alue) on tämän ytimen oma
 * raja blobin koolle, ei osa ABI:a.
 *
 * Kartoitus jää voimaan ytimen eliniäksi: confval[] (port/bootargs.c) ja
 * bootlogtext() (port/bootfb.c) osoittavat blobiin.  BOOTMAPVA jakaa
 * KZERO:n PML4-merkinnän, joten myös AP-suorittimet näkevät sen.
 */
uintptr bootinfopa;		/* l.s: talletetaan BSS:n nollauksen jälkeen */

enum {
	BootMapPages	= 2,
};
static uchar bootmapmem[(BootMapPages+1)*BY2PG];
static int bootmapused;

static uintptr*
bootmapnext(uintptr *pte)
{
	uintptr *t;

	if((*pte & PTEVALID) == 0){
		if(bootmapused >= BootMapPages)
			return nil;
		t = (uintptr*)(((uintptr)bootmapmem + BY2PG-1) & ~(BY2PG-1)) + bootmapused++*(BY2PG/sizeof(uintptr));
		memset(t, 0, BY2PG);
		*pte = PADDR(t) | PTEWRITE | PTEVALID;
	}
	return KADDR(*pte & ~(uintptr)(BY2PG-1) & ~PTENOEXEC);
}

/* kartoita [pa, pa+size) osoitteeseen BOOTMAPVA ja palauta se, tai nil; pa sivutasattu */
void*
bootearlymap(uvlong pa, uvlong size)
{
	uvlong o;
	uintptr va, *t;
	int l;

	if((pa & (BY2PG-1)) != 0 || size > BOOTMAPSIZE)
		return nil;
	for(o = 0; o < size; o += BY2PG){
		va = BOOTMAPVA + o;
		t = (uintptr*)CPU0PML4;
		for(l = 3; l > 0; l--)
			if((t = bootmapnext(&t[PTLX(va, l)])) == nil)
				return nil;
		t[PTLX(va, 0)] = (pa + o) | PTEWRITE | PTEVALID;
	}
	putcr3(getcr3());
	return (void*)BOOTMAPVA;
}

/*
 * Käynnistyskehyspuskuri (port/bootfb.c) write-combining-tilassa (PAT).
 * Näyttö on olemassa, joten conf.monitor kuten pc/screen.c:ssä.
 */
void*
fbmap(uvlong pa, uvlong size)
{
	void *v;

	if((v = vmap(pa, size)) == nil)
		return nil;
	patwc(v, size);
	conf.monitor = 1;
	return v;
}

/*
 * pc/memory.c:n e820scan() lukee UEFI-muistikartan tämän kautta
 * (bootmement) BIOS-aikaisen *e820-tekstin sijaan: kartta sellaisena kuin
 * loader sen otti juuri ennen ExitBootServicesia.  Merkintä i
 * e820-tyyppinä: 1 RAM, 3 ACPI, 2 varattu; tuntemattomat tyypit varattuina.
 * 0, kun merkintöjä ei ole enempää.
 */
int
bootinfoment(int i, uvlong *base, uvlong *top, int *type)
{
	BootMem *bm;

	if(bootinfo == nil || i < 0 || i >= bootinfo->mmapcount)
		return 0;
	bm = bootmem(i);
	*base = bm->base;
	*top = bm->base + bm->len;
	switch(bootmemclass(bm->type)){
	case BootClassRAM:
		*type = 1;
		break;
	case BootClassACPI:
		*type = 3;
		break;
	default:
		*type = 2;
	}
	return 1;
}

/*
 * Blobi on EfiLoaderDataa, jota kartta kutsuu RAM:ksi.  Se kuuluu ytimelle
 * pysyvästi (confval[] ja bootlogtext() osoittavat siihen), joten se
 * varataan meminit0():n jälkeen ennen kuin xinit() jakaa sivuja.
 */
void
bootinforeserve(void)
{
	if(bootinfo != nil)
		memreserve(bootinfopa, PGROUND((uvlong)bootinfo->totalsize));
}
