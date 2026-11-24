#include <u.h>
#include "fns.h"
#include "efi.h"
#include "mem.h"
#include "arm64layout.h"

/* ARM64: KZERO + physical address, X0=BootInfo.  The platform contract
 * (arm64layout.h) gives KZERO and the kernel's early direct map; the
 * kernel handles EL1/EL2 entry and builds its own translation tables.
 */

void jump(void *pc, void *bootinfo);

/* Initialized data addresses relocate with the EFI image (efi.c).
 * Globals taken by address go through linkfix(): 7l may address them
 * relative to R28 or by their link-time address, depending on layout. */
static ArmLayout layout = { .align = 4096 };
static uvlong scratchpa[ImageMaxSeg], scratchlen[ImageMaxSeg];
static int nscratch;

static int
rangeoverlap(uvlong a, uvlong n, uvlong b, uvlong m)
{
	return n!=0 && m!=0 && a<b+m && b<a+n;
}

static int
dynamicblocked(ArmLayout *l, BootImage *im, uvlong pa, uvlong len,
	uvlong blobpa, uvlong bloblen)
{
	int i;

	if(rangeoverlap(pa,len,l->scratchlo,l->scratchlen)
	|| rangeoverlap(pa,len,l->fdtlo,l->fdtlen)
	|| rangeoverlap(pa,len,blobpa,bloblen)) return 1;
	for(i=0;i<im->nseg;i++)
		if(rangeoverlap(pa,len,im->seg[i].pa,im->seg[i].allocsz)) return 1;
	for(i=0;i<l->nreserved;i++)
		if(rangeoverlap(pa,len,l->reservedlo[i],l->reservedlen[i])) return 1;
	return 0;
}

/* The platform is explicit configuration, not an inference from a DTB.
 * Firmware DTBs are not forwarded on ARM64 until a board-validated FDT
 * contract exists. BootInfo's optional FDT fields remain zero. */
int
archfdtallowed(void)
{
	/* Keep the bounded firmware candidate available until plan9.ini has
	 * explicitly selected a platform. rpi4 validates and copies it later. */
	return 1;
}

int
archfdtdefer(void)
{
	return 1;
}

/* the one value of key in plan9.ini; duplicates are rejected */
static char*
value(char *key, char *out, int cap)
{
	char *p, *e;
	int n, k, found;

	key = linkfix(key);
	n = conflen() - 1;
	k = strlen(key);
	found = 0;
	out[0] = 0;
	for(p = confaddr; p < confaddr+n; p = e+1){
		for(e = p; e < confaddr+n && *e != '\n'; e++)
			if(*e == 0)
				return "embedded NUL in ARM64 layout configuration";
		if(e-p < k || memcmp(p, key, k) != 0)
			continue;
		if(found++ || e-p-k >= cap)
			return "duplicate/oversized ARM64 layout setting";
		memmove(out, p+k, e-p-k);
		out[e-p-k] = 0;
	}
	return nil;
}

/* decimal or 0x hexadecimal, without overflow */
static int
number(char **text, uvlong *v)
{
	char *p;
	uvlong a;
	int base, d, n;

	p = *text;
	base = 10;
	a = 0;
	n = 0;
	if(p[0] == '0' && p[1] == 'x'){
		base = 16;
		p += 2;
	}
	for(;; p++){
		if(*p >= '0' && *p <= '9')
			d = *p - '0';
		else if(*p >= 'a' && *p <= 'f')
			d = *p - 'a' + 10;
		else if(*p >= 'A' && *p <= 'F')
			d = *p - 'A' + 10;
		else
			break;
		if(d >= base || a > (~(uvlong)0 - d) / base)
			return 0;
		a = a*base + d;
		n++;
	}
	if(n == 0)
		return 0;
	*v = a;
	*text = p;
	return 1;
}

/* "base,length" and nothing else */
static int
pair(char *s, uvlong *a, uvlong *n)
{
	if(!number(&s, a) || *s++ != ',' || !number(&s, n) || *s != 0)
		return 0;
	return 1;
}

/* release the early scratch reservations of a failed attempt */
void
archrelease(void)
{
	uvlong *pa = linkfix(scratchpa), *len = linkfix(scratchlen);

	while(nscratch > 0){
		nscratch--;
		efifree(pa[nscratch], len[nscratch]);
	}
}

char*
archinit(void)
{
	char platform[32], map[80], scratch[80], *why;
	uvlong rlo[ArmMaxReserved], rlen[ArmMaxReserved];
	ArmDynamic dynamic[ArmMaxDynamic];
	uvlong lo, len, sl, sn;
	int explicit, nr, nd, i;

	archrelease();
	if((why = value("efiplatform=", platform, sizeof platform)) != nil)
		return why;
	if((why = value("efibootmap=", map, sizeof map)) != nil)
		return why;
	if((why = value("efiscratch=", scratch, sizeof scratch)) != nil)
		return why;
	explicit = 0;
	lo = len = sl = sn = 0;
	if(map[0]){
		if(!pair(map, &lo, &len))
			return "invalid efibootmap (base,length required)";
		explicit |= 1;
	}
	if(scratch[0]){
		if(!pair(scratch, &sl, &sn))
			return "invalid efiscratch (base,length required)";
		explicit |= 2;
	}
	if((why = armplatform(linkfix(&layout), platform, explicit, lo, len, sl, sn)) != nil)
		return why;
	if(layout.platform == ArmPi4){
		if((why = armfdtvalidate(linkfix(fdtsrc), fdtsize)) != nil)
			return why;
		layout.fdtlo = (uvlong)(uintptr)fdtsrc;
		layout.fdtlen = fdtsize;
		if((why = armfdtreservations(linkfix(fdtsrc), fdtsize, rlo, rlen,
		    ArmMaxReserved, &nr, dynamic, ArmMaxDynamic, &nd)) != nil)
			return why;
		layout.nreserved = nr;
		for(i=0; i<nr; i++){
			layout.reservedlo[i] = rlo[i];
			layout.reservedlen[i] = rlen[i];
		}
		layout.ndynamic=nd;
		for(i=0;i<nd;i++) layout.dynamic[i]=dynamic[i];
		if((why = bootfdtcopy()) != nil)
			return why;
	}
	return nil;
}

/*
 * Validate the complete image plan against the platform contract, then
 * place the BootInfo blob and check every range against the firmware's
 * memory descriptors before any image byte is written.
 */
char*
archimage(BootImage *im)
{
	void *map;
	EFI_MEMORY_DESCRIPTOR *m;
	uvlong bytes, stride, pa, n, i, lo, hi, top;
	uvlong blobpa, bloblen, align, rangebase, rangelen, candidate, end;
	uvlong *savedpa = linkfix(scratchpa), *savedlen = linkfix(scratchlen);
	char *why;
	int j, k, placed;
	if((why = armimage(linkfix(&layout), im)) != nil)
		return why;
	if(bootmemorymap(&map, &bytes, &stride) != 0)
		return "cannot snapshot UEFI memory map";
	n = (bootblobsize() + layout.align-1) & ~(layout.align-1);
	if(!armblob(linkfix(&layout), im, map, bytes, stride, n, &pa))
		return "no conventional RAM for ARM64 BootInfo in declared map";
	if(bootblobmove(pa, n) != 0)
		return "cannot claim ARM64 BootInfo range";
	if(bootmemorymap(&map, &bytes, &stride) != 0)
		return "cannot refresh UEFI memory map";
	if((why = armmemory(linkfix(&layout), im, map, bytes, stride)) != nil)
		return why;
	/* Reserve conventional parts of the kernel's after-EBS scratch before
	 * kernel allocation. Boot-services portions are already non-allocatable
	 * in the final map; resident firmware types were rejected by armmemory. */
	for(i = 0; layout.scratchlen && i < bytes; i += stride){
		m = (EFI_MEMORY_DESCRIPTOR*)((uchar*)map + i);
		if(m->Type != EfiConventionalMemory)
			continue;
		lo = m->PhysicalStart;
		hi = lo + m->NumberOfPages*4096;
		top = layout.scratchlo + layout.scratchlen;
		if(lo < layout.scratchlo)
			lo = layout.scratchlo;
		if(hi > top)
			hi = top;
		if(lo >= hi)
			continue;
		if(nscratch == ImageMaxSeg || bootreserve(lo, hi-lo) != 0){
			archrelease();
			return "cannot reserve ARM64 early scratch";
		}
		savedpa[nscratch] = lo;
		savedlen[nscratch++] = hi - lo;
	}
	blobpa=bootblobbase(); bloblen=bootblobsize();
	for(j=0;j<layout.ndynamic;j++){
		ArmDynamic *d=&layout.dynamic[j];
		align=d->align?d->align:4096;
		if(align<4096 || (align&(align-1)) || d->size==0
		|| d->size>~(uvlong)0-4095)
			return "invalid dynamic FDT reservation alignment/size";
		n=(d->size+4095)&~(uvlong)4095;
		if(bootmemorymap(&map,&bytes,&stride)!=0)
			return "cannot refresh UEFI map for dynamic FDT reservation";
		placed=0;
		for(k=0;k<(d->nranges?d->nranges:1) && !placed;k++){
			rangebase=d->nranges?d->rangelo[k]:layout.maplo;
			rangelen=d->nranges?d->rangelen[k]:layout.maplen;
			if(rangelen==0 || rangebase>~(uvlong)0-rangelen) continue;
			end=rangebase+rangelen;
			for(i=0;i<bytes;i+=stride){
				m=(EFI_MEMORY_DESCRIPTOR*)((uchar*)map+i);
				if(m->Type!=EfiConventionalMemory || (m->Attribute&(1ULL<<63))) continue;
				lo=m->PhysicalStart; hi=lo+m->NumberOfPages*4096;
				if(lo<rangebase) lo=rangebase;
				if(hi>end) hi=end;
				if(hi<lo || hi-lo<n) continue;
				candidate=(hi-n)&~(align-1);
				while(candidate>=lo && candidate<=hi-n){
					if(!dynamicblocked(linkfix(&layout),im,candidate,n,blobpa,bloblen)
					&& bootreserve(candidate,n)==0){
						if(layout.nreserved==ArmMaxReserved){
							efifree(candidate,n);
							return "too many FDT/firmware reserved ranges";
						}
						layout.reservedlo[layout.nreserved]=candidate;
						layout.reservedlen[layout.nreserved++]=n;
						placed=1; break;
					}
					if(candidate<lo+align) break;
					candidate-=align;
				}
				if(placed) break;
			}
		}
		if(!placed) return "cannot place dynamic FDT reserved-memory pool";
	}
	return nil;
}

/* the ISA into the BootInfo; there is no TSC (its tscfreq stays 0) */
void
archconf(BootInfo *bi)
{
	bi->arch = BootArchArm64;
}

/* the kernel's physical entry from its entry: linked at KZERO + pa */
uvlong
archentry(uvlong entry)
{
	return entry - layout.kzero;
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
 * May the BootInfo blob lie at [pa, pa+len)?  Provisional allocation only:
 * archimage() rehomes it using the actual image plan and firmware
 * descriptors before claiming or writing any image.
 */
int
archblobok(uvlong pa, uvlong len)
{
	return pa != 0 && len != 0 && pa <= ~(uvlong)0 - len;
}

/* ELF-ytimen e_machine (sub.c bootelf()): EM_AARCH64 */
int
archelfmachine(void)
{
	return 183;
}

/* the kernel's entry must be in RAM the declared boot map covers */
int
archentryok(uvlong entry)
{
	uvlong pa;

	if(!layout.platform || entry < layout.kzero)
		return 0;
	pa = entry - layout.kzero;
	return pa >= layout.maplo && pa - layout.maplo < layout.maplen;
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
