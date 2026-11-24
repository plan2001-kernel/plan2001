/*
 * Raspberry Pi 4 käynnistettynä UEFI:n kautta (CONF=pi4efi, -DUEFI):
 * VideoCore-firmware -> RPI_EFI.fd (TF-A EL3:ssa, EDK2) -> Plan2001:n
 * EFI-loader -> tämä ydin, X0 = BootInfo v1:n fyysinen osoite.
 *
 * Loaderin ja ytimen sopimus (bcm64/mem.h, mkfile, docs/rpi4.md):
 *	- ydin on 9frontin arm64 a.out, teksti fyysisessä osoitteessa
 *	  entry - KZERO (KZERO 0xffffffffc0000000), data seuraavalla
 *	  64 KiB:n rajalla, bss sen perässä;
 *	- loader varaa myös tekstin alapuolisen alueen
 *	  [PHYSBASE, entry-KZERO): siinä ovat stock-asettelun mukaiset
 *	  käynnistysargumentit, mailbox-puskuri, sivutaulut ja Mach-rakenteet;
 *	- BootInfo-blob ja sen FDT-kopio ovat kokonaan alle 1 GiB:n
 *	  (alkukartoitus), eivätkä ne ole ytimen alueella;
 *	- EL2 tai EL1, keskeytykset pois, välimuistit puhdistettu (aa64.s).
 *
 * Kaikki tarkistetaan täällä; ristiriidasta ilmoitetaan PL011:een ja
 * pysähdytään.  Stock-firmwaren DTB-sisääntuloon (9pi4) ei siirrytä.
 */
#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "../port/error.h"
#include "ureg.h"

#define BOOTARGS	((char*)CONFADDR)
#define BOOTARGSLEN	((KZERO+REBOOTADDR)-CONFADDR)

/* PSCI 1.x (Arm DEN0022), SMC64/SMC32-funktiot */
enum {
	PsciVersion	= 0x84000000,
	PsciCpuOff	= 0x84000002,
	PsciCpuOn	= 0xC4000003,
	PsciSystemOff	= 0x84000008,
	PsciSystemReset	= 0x84000009,
};

uintptr bootinfopa;

static void (*psci)(Ureg*);		/* nil: ei PSCI:tä, yksi ydin */
static char *pscimethod = "none";
static ulong pscivers;
static u64int cpureg[MAXMACH];		/* MPIDR-affiniteetti, /cpus/cpu@N reg */
static int ncpureg;
static int bcm2711;
static char derived[512];		/* laitepuusta johdetut *-muuttujat */

/*
 * Varhainen tulostus suoraan PL011:een (EDK2:n konsoli GPIO14/15:ssä,
 * dtoverlay=miniuart-bt), ennen ytimen omaa konsolia.  VIRTIO on
 * kartoitettu jo l.s:ssä (mmu0init).
 */
enum {
	Uartdr	= 0x00>>2,
	Uartfr	= 0x18>>2,
		Txff	= 1<<5,
	Uartcr	= 0x30>>2,
		Uarten	= 1<<0,
};

static void
earlyputc(int c)
{
	u32int *r = (u32int*)(VIRTIO+0x201000);
	int i;

	if((r[Uartcr] & Uarten) == 0)
		return;
	for(i = 0; i < 1000000 && (r[Uartfr] & Txff) != 0; i++)
		;
	r[Uartdr] = c;
}

static void
earlyputs(char *s)
{
	for(; *s != 0; s++){
		if(*s == '\n')
			earlyputc('\r');
		earlyputc(*s);
	}
}

static void
earlyhex(uvlong v)
{
	char buf[19];
	int i;

	buf[0] = '0';
	buf[1] = 'x';
	for(i = 0; i < 16; i++)
		buf[2+i] = "0123456789abcdef"[(v >> (60 - 4*i)) & 0xF];
	buf[18] = 0;
	earlyputs(buf);
}

/* ristiriita sopimuksessa: ilmoitus ja pysähdys, ei muuta reittiä */
static void
uefidie(char *why, uvlong v)
{
	earlyputs("\npi4efi: ");
	earlyputs(why);
	if(v != ~0ULL){
		earlyputs(" ");
		earlyhex(v);
	}
	earlyputs("\npi4efi: halted (docs/rpi4.md)\n");
	halt();
}

void*
bootearlymap(uvlong pa, uvlong size)
{
	/* l.s kartoittaa KZERO:sta 1 GiB (mmu0init) */
	if((pa & (BY2PG-1)) != 0 || pa >= (uintptr)-KZERO
	|| size > (uintptr)-KZERO - pa)
		return nil;
	return (void*)(uintptr)(KZERO + pa);
}

void
halt(void)
{
	splfhi();
	for(;;)
		idlehands();
}

static u32int
be32(uchar *p)
{
	return (u32int)p[0]<<24 | (u32int)p[1]<<16 | (u32int)p[2]<<8 | (u32int)p[3];
}

static uvlong
be64(uchar *p)
{
	return (uvlong)be32(p)<<32 | be32(p+4);
}

/* onko s NUL-erotelluissa merkkijonoissa v[0:n] */
static int
strlistin(char *v, int n, char *s)
{
	char *e;

	for(e = v + n; v < e; v += strlen(v) + 1)
		if(memchr(v, 0, e - v) == nil)
			return 0;
		else if(strcmp(v, s) == 0)
			return 1;
	return 0;
}

static int
strprefixin(char *v, int n, char *s)
{
	char *e;

	for(e = v + n; v < e; v += strlen(v) + 1)
		if(memchr(v, 0, e - v) == nil)
			return 0;
		else if(strncmp(v, s, strlen(s)) == 0)
			return 1;
	return 0;
}

/*
 * Laitepuusta tämän ytimen tarvitsemat: levy (brcm,bcm2711), PSCI:n
 * kanava, ytimien MPIDR:t ja BCM2711:n DMA-ikkunat, jotka riippuvat
 * piirin versiosta (samat kuin ../bcm/bootargs.c lukee stock-DTB:stä).
 */
static char emmc2bus[40], pciwin[40], pcidmawin[40];

static void
fdtprop(char *path, char *key, uchar *v, int n)
{
	uvlong addr, size;

	if(strcmp(path, "") == 0 && strcmp(key, "compatible") == 0){
		bcm2711 = strlistin((char*)v, n, "brcm,bcm2711");
		return;
	}
	if(strcmp(path, "/psci") == 0 && strcmp(key, "method") == 0){
		if(n == 4 && memcmp(v, "smc", 4) == 0){
			psci = smccall;
			pscimethod = "smc";
		}else if(n == 4 && memcmp(v, "hvc", 4) == 0){
			psci = hvccall;
			pscimethod = "hvc";
		}
		return;
	}
	if(strncmp(path, "/cpus/cpu@", 10) == 0 && strcmp(key, "reg") == 0){
		if(ncpureg < MAXMACH && (n == 4 || n == 8))
			cpureg[ncpureg++] = n == 4 ? be32(v) : be64(v);
		return;
	}
	if(strcmp(path, "/emmc2bus") == 0 && strcmp(key, "dma-ranges") == 0 && n == 5*4){
		addr = be64(v) - be64(v+8);
		size = be32(v+16);
		snprint(emmc2bus, sizeof emmc2bus, "%#llux %#llux", addr, addr+size);
		return;
	}
	if(strncmp(path, "/scb/pcie@", 10) == 0 && n == 7*4 && (be32(v) & 0x3000000) == 0x2000000){
		size = be64(v+5*4);
		if(strcmp(key, "ranges") == 0){
			addr = be64(v+3*4);
			snprint(pciwin, sizeof pciwin, "%#llux %#llux", addr, addr+size);
		}else if(strcmp(key, "dma-ranges") == 0){
			addr = be64(v+1*4) - be64(v+3*4);
			snprint(pcidmawin, sizeof pcidmawin, "%#llux %#llux", addr, addr+size);
		}
	}
}

enum {
	FdtBegin	= 1,
	FdtEnd		= 2,
	FdtProp		= 3,
	FdtNop		= 4,
	FdtFinish	= 9,
};

/* rakennelohkon läpikäynti rajat tarkistaen; -1 jos rikki */
static int
fdtwalk(uchar *fdt, ulong len)
{
	uchar *p, *e, *stab;
	ulong off, stlen, stoff;
	char path[256], *s;
	int depth, n, plen[16];
	u32int t;

	if(len < 40 || be32(fdt) != 0xd00dfeed || be32(fdt+4) > len)
		return -1;
	len = be32(fdt+4);
	off = be32(fdt+8);
	stoff = be32(fdt+12);
	stlen = be32(fdt+32);
	if(off >= len || stoff > len || stlen > len - stoff)
		return -1;
	p = fdt + off;
	e = fdt + len;
	stab = fdt + stoff;
	depth = 0;
	path[0] = 0;
	while(p + 4 <= e){
		t = be32(p);
		p += 4;
		switch(t){
		case FdtBegin:
			s = (char*)p;
			if(memchr(s, 0, e - p) == nil)
				return -1;
			n = strlen(s);
			if(depth >= nelem(plen))
				return -1;
			plen[depth++] = strlen(path);
			if(depth > 1){
				if(strlen(path) + 1 + n >= sizeof path)
					return -1;
				strcat(path, "/");
				strcat(path, s);
			}
			p += (n + 4) & ~3;
			break;
		case FdtEnd:
			if(depth == 0)
				return -1;
			path[plen[--depth]] = 0;
			break;
		case FdtProp:
			if(p + 8 > e)
				return -1;
			n = be32(p);
			off = be32(p+4);
			p += 8;
			if(n < 0 || n > e - p || off >= stlen || memchr(stab+off, 0, stlen-off) == nil)
				return -1;
			if(depth > 0)
				fdtprop(path, (char*)stab+off, p, n);
			p += (n + 3) & ~3;
			break;
		case FdtNop:
			break;
		case FdtFinish:
			return depth == 0 ? 0 : -1;
		default:
			return -1;
		}
	}
	return -1;
}

/* onko [lo, hi) kokonaan muistikartan loaderin (LoaderCode/Data) alueilla */
static int
loaderowns(uvlong lo, uvlong hi)
{
	BootMem *bm;
	int i, more;

	do {
		more = 0;
		for(i = 0; i < bootinfo->mmapcount && lo < hi; i++){
			bm = bootmem(i);
			if(bm->type != BootMemLoaderCode && bm->type != BootMemLoaderData)
				continue;
			if(bm->base <= lo && lo < bm->base + bm->len){
				lo = bm->base + bm->len;
				more = 1;
			}
		}
	} while(more && lo < hi);
	return lo >= hi;
}

void
uefibootinit(void)
{
	uchar *h, *fdt;
	ulong fdtlen;
	uvlong klo, khi;
	char *p, *e, *c;
	int n;

	if(bootinfopa == 0)
		uefidie("X0 was 0 at entry: no BootInfo", ~0ULL);
	if((h = bootearlymap(bootinfopa, BY2PG)) == nil)
		uefidie("X0 is not a page-aligned address below 1 GiB:", bootinfopa);
	if(be32(h) == 0xd00dfeed)
		uefidie("X0 is a device tree, not BootInfo: the Pi firmware boots 9pi4, the Plan2001 EFI loader 9pi4efi", ~0ULL);
	if(((BootInfo*)h)->magic != BootInfoMagic)
		uefidie("X0 is not a BootInfo blob, magic", ((BootInfo*)h)->magic);
	if(((BootInfo*)h)->version != BootInfoVersion)
		uefidie("BootInfo version not 1:", ((BootInfo*)h)->version);
	if(((BootInfo*)h)->headersize >= (uintptr)&((BootInfo*)0)->fdtoff
	&& ((BootInfo*)h)->arch != BootArchArm64)
		uefidie("BootInfo made for another architecture:", ((BootInfo*)h)->arch);
	earlyputs("pi4efi: BootInfo v1 at ");
	earlyhex(bootinfopa);
	earlyputs("\n");

	/* yhteinen tarkistus (port/bootinfo.c) pysähtyy kaikkeen muuhun */
	bootinfoinit();

	/* ytimen alue on loaderin varaama, eikä blob ole sen päällä */
	klo = PHYSBASE;
	khi = PGROUND(PADDR(end));
	if(!loaderowns(klo, khi))
		uefidie("the loader did not claim the kernel's range from PHYSBASE", klo);
	if(bootinfopa < khi && klo < bootinfopa + bootinfo->totalsize)
		uefidie("BootInfo overlaps the kernel at", bootinfopa);

	fdt = bootfdt(&fdtlen);
	if(fdt == nil)
		uefidie("no device tree in BootInfo: EDK2 System Table Selection must include Devicetree", ~0ULL);
	if(fdtwalk(fdt, fdtlen) < 0)
		uefidie("device tree in BootInfo is not well formed", ~0ULL);
	if(!bcm2711)
		uefidie("device tree is not a BCM2711 (Raspberry Pi 4) one", ~0ULL);

	if(psci != nil){
		Ureg u;

		memset(&u, 0, sizeof u);
		u.r0 = PsciVersion;
		(*psci)(&u);
		if((vlong)u.r0 < 0x10000){
			psci = nil;
			pscimethod = "unsupported";
		} else
			pscivers = u.r0;
	}

	/* laitepuusta johdetut ensin; plan9.ini (BootInfo) voi korvata ne */
	p = derived;
	e = derived + sizeof derived;
	p = seprint(p, e, "*bootinfo=%#p\n", bootinfopa);
	p = seprint(p, e, "*ncpu=%d\n", psci != nil && ncpureg > 0 ? ncpureg : 1);
	if(emmc2bus[0])
		p = seprint(p, e, "*emmc2bus=%s\n", emmc2bus);
	if(pciwin[0])
		p = seprint(p, e, "*pciwin=%s\n", pciwin);
	if(pcidmawin[0])
		seprint(p, e, "*pcidmawin=%s\n", pcidmawin);

	c = bootconfig();
	n = strlen(derived);
	if(n + strlen(c) + 1 > BOOTARGSLEN)
		uefidie("plan9.ini does not fit the boot arguments, bytes", strlen(c));
	memmove(BOOTARGS, derived, n);
	strcpy(BOOTARGS + n, c);
	bootargsinit(0);	/* ../bcm/bootargs.c: plan9.ini-muoto BOOTARGSista */
	bootinforandinit();
}

/*
 * Muisti BootInfon kartasta: vapaa (Conventional) ja käynnistyspalvelujen
 * muisti (BootCode/Data) on ytimen.  Loaderin alueet (ydin, blob, levykuva)
 * ja firmware (TF-A, EDK2:n ajonaikaiset, VideoCore) jäävät pois.
 */
enum { Nbank = 64 };

void
uefimeminit(void)
{
	static struct { uvlong base, limit; } bank[Nbank], t;
	BootMem *bm;
	uvlong b, l, top;
	int i, j, n;

	n = 0;
	top = (uvlong)PHYSDRAM + soc.dramsize;
	if(top > KMAPEND - KMAP)
		top = KMAPEND - KMAP;
	for(i = 0; i < bootinfo->mmapcount; i++){
		bm = bootmem(i);
		if(bm->type != BootMemConventional && bm->type != BootMemBootCode
		&& bm->type != BootMemBootData)
			continue;
		b = PGROUND(bm->base);
		l = (bm->base + bm->len) & ~(uvlong)(BY2PG-1);
		if(l > top)
			l = top;
		if(b >= l)
			continue;
		/* järjestyksessä, vierekkäiset yhdistettyinä */
		for(j = n; j > 0 && bank[j-1].base > b; j--)
			;
		if(j > 0 && bank[j-1].limit == b){
			bank[j-1].limit = l;
		} else if(j < n && bank[j].base == l){
			bank[j].base = b;
		} else if(n < Nbank){
			memmove(&bank[j+1], &bank[j], (n-j)*sizeof bank[0]);
			bank[j].base = b;
			bank[j].limit = l;
			n++;
		}
		for(j = 1; j < n; j++)
			if(bank[j-1].limit == bank[j].base){
				bank[j-1].limit = bank[j].limit;
				memmove(&bank[j], &bank[j+1], (n-j-1)*sizeof bank[0]);
				n--;
				j--;
			}
	}
	/* suurimmat nelem(conf.mem) alkuun, sitten osoitejärjestykseen */
	for(i = 0; i < n; i++)
		for(j = i+1; j < n; j++)
			if(bank[j].limit - bank[j].base > bank[i].limit - bank[i].base){
				t = bank[i];
				bank[i] = bank[j];
				bank[j] = t;
			}
	if(n > nelem(conf.mem))
		n = nelem(conf.mem);
	for(i = 0; i < n; i++)
		for(j = i+1; j < n; j++)
			if(bank[j].base < bank[i].base){
				t = bank[i];
				bank[i] = bank[j];
				bank[j] = t;
			}
	memset(conf.mem, 0, sizeof conf.mem);
	l = 0;
	for(i = 0; i < n; i++){
		conf.mem[i].base = bank[i].base;
		conf.mem[i].limit = bank[i].limit;
		kmapram(conf.mem[i].base, conf.mem[i].limit);
		conf.mem[i].npage = (conf.mem[i].limit - conf.mem[i].base)/BY2PG;
		l += conf.mem[i].npage;
	}
	flushtlb();
	if(l < 64*MB/BY2PG)
		uefidie("BootInfo memory map has under 64 MiB of usable RAM, pages", l);
}

void
uefiprint(void)
{
	ulong fdtlen;
	int i;

	bootfdt(&fdtlen);
	print("pi4efi: BootInfo v1 %#p, %d memory map entries, device tree %lud bytes\n",
		bootinfopa, bootinfo->mmapcount, fdtlen);
	print("pi4efi: kernel %#llux-%#p, psci %s %#lux, %d cpus in device tree\n",
		(uvlong)PHYSBASE, PADDR(end), pscimethod, pscivers, ncpureg);
	for(i = 0; i < nelem(conf.mem); i++)
		if(conf.mem[i].npage)
			print("pi4efi: memory %#p-%#p\n", conf.mem[i].base, conf.mem[i].limit);
}

/*
 * Toissijaiset ytimet PSCI CPU_ON:lla, TF-A:n kautta (ei armstubin
 * spin-taulua, jota EDK2:n alla ei ole).  Ydin tulee _startiin MMU ja
 * välimuistit pois, EL2:ssa tai EL1:ssä; l.s laskee Mach-rakenteen
 * MPIDR:stä kuten stock-käynnistyksessä.
 */
void
uefimpinit(void)
{
	extern void _start(void);
	extern int mpidindex(uvlong);
	Ureg u;
	int i, t;

	if(psci == nil || conf.nmach <= 1)
		return;
	/*
	 * Yksi kerrallaan: synccycles() odottaa conf.nmach ydintä, joten
	 * käynnistymätön ydin rajaa conf.nmach:n ennen sitä.  Ydin, jonka
	 * PSCI käynnisti mutta joka ei tule, voisi tulla myöhemmin: panic.
	 */
	for(i = 1; i < conf.nmach; i++){
		if(i >= ncpureg || mpidindex(cpureg[i] & MPIDMASK) != i){
			print("pi4efi: cpu%d: no matching /cpus reg\n", i);
			break;
		}
		MACHP(i)->machno = i;
		cachedwbinvse(MACHP(i), MACHSIZE);
		memset(&u, 0, sizeof u);
		u.r0 = PsciCpuOn;
		u.r1 = cpureg[i];
		u.r2 = PADDR(_start);
		(*psci)(&u);
		if(u.r0 != 0){
			print("pi4efi: cpu%d: PSCI CPU_ON %lld\n", i, (vlong)u.r0);
			break;
		}
		/* _start odottaa WFE:ssä (l.s _startup) */
		for(t = 0; t < 1000 && active.machs[i] == 0; t++){
			sev();
			delay(1);
		}
		if(active.machs[i] == 0)
			panic("pi4efi: cpu%d did not arrive after PSCI CPU_ON", i);
	}
	conf.nmach = i;
	synccycles();
}

/* exit(): toissijainen ydin pois, ensisijainen nollaa koneen (PSCI) */
void
uefiexit(void)
{
	Ureg u;

	if(psci == nil){
		if(m->machno == 0)
			archreboot();
		halt();
	}
	memset(&u, 0, sizeof u);
	u.r0 = m->machno ? PsciCpuOff : PsciSystemReset;
	(*psci)(&u);
	if(m->machno == 0)
		archreboot();
	halt();
}
