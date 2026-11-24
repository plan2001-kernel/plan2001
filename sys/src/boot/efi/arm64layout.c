#include <u.h>
#include "fns.h"
#include "efi.h"
#include "arm64layout.h"

/* b is a literal: see linkfix() in efi.c */
static int
equal(char *a, char *b)
{
	b = linkfix(b);
	return strlen(a)==strlen(b) && memcmp(a,b,strlen(a))==0;
}

static int
span(uvlong a, uvlong n)
{
	return n != 0 && a <= ~(uvlong)0-n;
}

static int
overlap(uvlong a, uvlong n, uvlong b, uvlong m)
{
	return n && m && a < b+m && b < a+n;
}

char*
armplatform(ArmLayout *l, char *name, int explicit, uvlong lo, uvlong len,
	uvlong scratch, uvlong slen)
{
	memset(l, 0, sizeof *l);
	if(equal(name,"qemu-virt")){
		if(explicit)
			return "QEMU uses its frozen image/mapping contract";
		l->platform=ArmQemu;
		l->kzero=0xffffffff80000000ULL;
		l->maplo=0x40000000ULL;
		l->maplen=0x40000000ULL;
		/* arm64/mem.h: [L1BOT-KZERO, MACHADDR(-1)-KZERO). */
		l->scratchlo=0x400d5000ULL;
		l->scratchlen=0x2b000;
		l->align=4096;
		return nil;
	}
	if(!equal(name,"rpi4"))
		return "unknown or missing ARM64 efiplatform";
	if(explicit != 3)
		return "rpi4 requires efibootmap and efiscratch";
	if(!span(lo,len) || lo+len > 0x40000000ULL
	|| ((lo|len|scratch|slen)&65535) != 0)
		return "invalid rpi4 direct-map contract";
	if((slen==0 && scratch!=0) || (slen!=0 &&
	   (!span(scratch,slen) || scratch<lo || scratch+slen>lo+len)))
		return "invalid rpi4 early scratch contract";
	l->platform=ArmPi4;
	l->kzero=0xffffffffc0000000ULL;
	l->maplo=lo;
	l->maplen=len;
	l->scratchlo=scratch;
	l->scratchlen=slen;
	l->align=65536;
	return nil;
}

char*
armimage(ArmLayout *l, BootImage *im)
{
	BootSegment *s, *p;
	uvlong n;
	int i,j,entry;

	if(l->platform==0 || im->nseg<1 || im->nseg>ImageMaxSeg)
		return "missing ARM64 image layout";
	if(im->format==ImageAout){
		if(l->platform!=ArmQemu || im->magic!=0x8c47
		|| im->nseg!=1 || im->entry!=0xffffffffc0100000ULL)
			return "unsupported ARM64 a.out (rpi4 requires linked ELF64)";
	}else if(im->format!=ImageElf)
		return "unsupported ARM64 image format";
	entry=0;
	for(i=0;i<im->nseg;i++){
		s=&im->seg[i];
		if(!span(s->pa,s->memsz) || s->filesz>s->memsz
		|| (s->pa&(l->align-1)) || s->memsz>~(uvlong)0-(l->align-1))
			return "invalid ARM64 load segment";
		n=(s->memsz+l->align-1)&~(l->align-1);
		s->allocsz=n;
		if(!span(s->pa,n) || s->pa<l->maplo || s->pa>=l->maplo+l->maplen
		|| n>l->maplo+l->maplen-s->pa || s->pa>~(uvlong)0-l->kzero
		|| s->va!=l->kzero+s->pa || !span(s->va,s->memsz))
			return "ARM64 segment outside declared direct map";
		if(overlap(s->pa,n,l->scratchlo,l->scratchlen))
			return "ARM64 load segment overlaps early scratch";
		for(j=0;j<i;j++){
			p=&im->seg[j];
			if(overlap(s->pa,n,p->pa,p->allocsz))
				return "overlapping ARM64 load allocations";
		}
		if((s->flags&ImageExec) && s->filesz && im->entry>=s->va
		&& im->entry-s->va<s->filesz){
			if(entry++)
				return "ambiguous ARM64 entry";
			im->entrypa=s->pa+(im->entry-s->va);
		}
	}
	if(!entry || (im->entry&3))
		return "ARM64 entry is not file-backed executable code";
	return nil;
}

char*
armmapvalid(void *map, uvlong bytes, uvlong stride)
{
	EFI_MEMORY_DESCRIPTOR *m,*p;
	uvlong i,j,len;

	if(map==nil || stride<sizeof(*m) || stride>bytes || bytes%stride)
		return "invalid UEFI memory descriptor size";
	for(i=0;i<bytes;i+=stride){
		m=(EFI_MEMORY_DESCRIPTOR*)((uchar*)map+i);
		if((m->PhysicalStart&4095) || m->NumberOfPages>~(uvlong)0/4096)
			return "invalid UEFI physical range";
		len=m->NumberOfPages*4096;
		if(len==0) continue;
		if(!span(m->PhysicalStart,len))
			return "overflowing UEFI physical range";
		for(j=0;j<i;j+=stride){
			p=(EFI_MEMORY_DESCRIPTOR*)((uchar*)map+j);
			if(overlap(m->PhysicalStart,len,p->PhysicalStart,p->NumberOfPages*4096))
				return "overlapping UEFI memory descriptors";
		}
	}
	return nil;
}

/* Image bytes are written before EBS: only conventional RAM is safe.
 * Scratch is written by the kernel after EBS, so boot-services RAM is
 * also reclaimable. Runtime/reserved/MMIO/loader ranges never qualify. */
static int
armrange(void *map, uvlong bytes, uvlong stride, uvlong pa, uvlong len, int scratch)
{
	EFI_MEMORY_DESCRIPTOR *m;
	uvlong i,end,top;
	int found;

	if(!span(pa,len)) return 0;
	end=pa+len;
	while(pa<end){
		found=0;
		for(i=0;i<bytes;i+=stride){
			m=(EFI_MEMORY_DESCRIPTOR*)((uchar*)map+i);
			top=m->PhysicalStart+m->NumberOfPages*4096;
			if(pa<m->PhysicalStart || pa>=top) continue;
			if((m->Attribute&(1ULL<<63)) ||
			   (m->Type!=EfiConventionalMemory && (!scratch ||
			    (m->Type!=EfiBootServicesCode && m->Type!=EfiBootServicesData))))
				return 0;
			pa=top<end?top:end;
			found=1;
			break;
		}
		if(!found) return 0;
	}
	return 1;
}

char*
armmemory(ArmLayout *l, BootImage *im, void *map, uvlong bytes, uvlong stride)
{
	char *why;
	int i;

	if((why=armmapvalid(map,bytes,stride))!=nil) return why;
	for(i=0;i<im->nseg;i++)
		if(!armrange(map,bytes,stride,im->seg[i].pa,im->seg[i].allocsz,0))
			return "ARM64 image would overwrite firmware/reserved memory";
	if(l->scratchlen && !armrange(map,bytes,stride,l->scratchlo,l->scratchlen,1))
		return "ARM64 scratch would overwrite resident firmware";
	return nil;
}

int
armblob(ArmLayout *l, BootImage *im, void *map, uvlong bytes, uvlong stride,
	uvlong len, uvlong *out)
{
	EFI_MEMORY_DESCRIPTOR *m;
	uvlong i,lo,hi,a,n,cut;
	int j,k,hit;

	if(!l->platform || !l->align || !len || len>~(uvlong)0-(l->align-1) || armmapvalid(map,bytes,stride)!=nil)
		return 0;
	n=(len+l->align-1)&~(l->align-1);
	for(i=0;i<bytes;i+=stride){
		m=(EFI_MEMORY_DESCRIPTOR*)((uchar*)map+i);
		if(m->Type!=EfiConventionalMemory || (m->Attribute&(1ULL<<63))) continue;
		lo=m->PhysicalStart;
		hi=lo+m->NumberOfPages*4096;
		if(lo<l->maplo) lo=l->maplo;
		if(lo<4096) lo=4096;
		if(hi>l->maplo+l->maplen) hi=l->maplo+l->maplen;
		for(k=0;k<=im->nseg+1;k++){
			if(hi<lo || hi-lo<n) break;
			a=(hi-n)&~(l->align-1);
			if(a<lo) break;
			hit=0; cut=hi;
			for(j=0;j<im->nseg;j++)
				if(overlap(a,n,im->seg[j].pa,im->seg[j].allocsz)){
					if(im->seg[j].pa<cut) cut=im->seg[j].pa;
					hit=1;
				}
			if(overlap(a,n,l->scratchlo,l->scratchlen)){
				if(l->scratchlo<cut) cut=l->scratchlo;
				hit=1;
			}
			if(!hit){ *out=a; return 1; }
			hi=cut;
		}
	}
	return 0;
}
