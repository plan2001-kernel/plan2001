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

static ulong
fbe32(uchar *p)
{
	return (ulong)p[0]<<24 | (ulong)p[1]<<16 | (ulong)p[2]<<8 | p[3];
}

static int
fcmp(char *a, char *b)
{
	return strlen(a)==strlen(b) && memcmp(a,b,strlen(a))==0;
}

static int
fncmp(char *a, char *b, int n)
{
	return strlen(a)>=n && memcmp(a,b,n)==0;
}

static int
fappend(char *dst, char *src)
{
	int n, m;

	n=strlen(dst); m=strlen(src);
	if(n+m>=256) return 0;
	memmove(dst+n,src,m+1);
	return 1;
}

/* Validate a bounded BCM2711 firmware tree before it is copied into the
 * kernel-owned BootInfo blob. This deliberately accepts only the board
 * data consumed by bcm64/uefi.c. */
char*
armfdtvalidate(void *vfdt, uvlong avail)
{
	uchar *fdt, *p, *end, *strings, *resv, *rend, *v;
	ulong total, offstruct, offstrings, offresv, structlen, stringslen;
	ulong n, noff, token, pos;
	char path[256], names[16][64], *name;
	int depth, root, bcm, cpu, emmc, pcie, ended;
	uvlong addr, size;

	fdt = vfdt;
	if(fdt == nil || avail < 40 || fbe32(fdt) != 0xd00dfeed)
		return "missing or invalid firmware FDT header";
	total = fbe32(fdt+4);
	offstruct = fbe32(fdt+8);
	offstrings = fbe32(fdt+12);
	offresv = fbe32(fdt+16);
	stringslen = fbe32(fdt+32);
	structlen = fbe32(fdt+36);
	if(total < 40 || total > avail || total > 2*1024*1024
	|| fbe32(fdt+20)<17 || fbe32(fdt+24)>17 || fbe32(fdt+24)>fbe32(fdt+20)
	|| offstruct > total || structlen > total-offstruct
	|| offstrings > total || stringslen > total-offstrings
	|| offresv < 40 || offresv > total || (offresv&7)
	|| (offstruct&3) || offstruct < offresv)
		return "firmware FDT has invalid block bounds";
	/* The reservation map is terminated by an all-zero address/size pair. */
	resv = fdt+offresv;
	rend = fdt+offstruct;
	for(;;){
		if(resv+16 > rend)
			return "firmware FDT reservation map is unterminated";
		addr = ((uvlong)fbe32(resv)<<32) | fbe32(resv+4);
		size = ((uvlong)fbe32(resv+8)<<32) | fbe32(resv+12);
		resv += 16;
		if(addr == 0 && size == 0)
			break;
		if(size == 0 || addr > ~(uvlong)0-size)
			return "firmware FDT has an invalid reserved range";
	}
	if(offstrings < offstruct+structlen || offstrings < (ulong)(resv-fdt))
		return "firmware FDT blocks overlap";
	strings = fdt+offstrings;
	p = fdt+offstruct;
	end = p+structlen;
	depth = root = bcm = cpu = emmc = pcie = ended = 0;
	path[0] = 0;
	while(p+4 <= end){
		token = fbe32(p); p += 4;
		if(token == 1){	/* FDT_BEGIN_NODE */
			for(pos=0; p+pos<end && p[pos]!=0; pos++)
				;
			if(p+pos>=end || depth>=16 || pos>=sizeof names[0])
				return "firmware FDT has an invalid node name/depth";
			memmove(names[depth], p, pos);
			names[depth][pos]=0;
			if(depth == 0){
				if(pos != 0) return "firmware FDT root node is not empty";
				root=1;
				path[0]=0;
			}else{
				if(strlen(path)+pos+2>=sizeof path)
					return "firmware FDT node path is too long";
				if(!fappend(path,"/") || !fappend(path,names[depth]))
					return "firmware FDT node path is too long";
			}
			depth++;
			pos=(pos+4)&~3;
			if(pos>(ulong)(end-p)) return "firmware FDT node name exceeds structure block";
			p += pos;
		}else if(token == 2){	/* FDT_END_NODE */
			if(depth==0) return "firmware FDT has an unmatched end node";
			depth--;
			if(depth==0) path[0]=0;
			else{
				for(pos=strlen(path); pos>0 && path[pos-1]!='/'; pos--)
					;
				if(pos>0) path[pos-1]=0;
			}
		}else if(token == 3){	/* FDT_PROP */
			if(depth==0 || p+8>end) return "firmware FDT has a truncated property";
			n=fbe32(p); noff=fbe32(p+4); p+=8;
			if(n>(ulong)(end-p) || noff>=stringslen)
				return "firmware FDT property is outside its block";
			name=(char*)strings+noff;
			for(pos=0; noff+pos<stringslen && name[pos]!=0; pos++)
				;
			if(noff+pos>=stringslen) return "firmware FDT property name is unterminated";
			v=p;
			if(fcmp(path,"") && fcmp(name,"compatible")){
				for(pos=0; pos<n;){
					for(size=pos; size<n && v[size]!=0; size++) ;
					if(size==n) return "firmware FDT compatible list is unterminated";
					if(size-pos==12 && memcmp(v+pos,"brcm,bcm2711",12)==0) bcm=1;
					pos=size+1;
				}
			}else if(fcmp(path,"/psci") && fcmp(name,"method")){
				if(!((n==4 && memcmp(v,"smc\0",4)==0) ||
				     (n==4 && memcmp(v,"hvc\0",4)==0)))
					return "firmware FDT has an invalid PSCI conduit";
			}else if(fncmp(path,"/cpus/cpu@",10) && fcmp(name,"reg")){
				if(n==4 || n==8) cpu=1;
			}else if(fcmp(path,"/emmc2bus") && fcmp(name,"dma-ranges")){
				if(n==20) emmc=1;
			}else if(fncmp(path,"/scb/pcie@",10)){
				if((fcmp(name,"ranges") || fcmp(name,"dma-ranges")) && n==28)
					pcie |= fcmp(name,"ranges") ? 1 : 2;
			}
			pos=(n+3)&~3;
			if(pos>(ulong)(end-p)) return "firmware FDT property exceeds structure block";
			p += pos;
		}else if(token==4){	/* FDT_NOP */
		}else if(token==9){	/* FDT_END */
			if(depth!=0 || p!=end) return "firmware FDT has malformed end token";
			ended=1; break;
		}else
			return "firmware FDT has an unknown structure token";
	}
	if(!ended || !root || !bcm || !cpu || !emmc || pcie!=3)
		return "firmware FDT does not describe the required BCM2711 board";
	return nil;
}

static uvlong
fdtcells(uchar *p, int n)
{
	uvlong v=0;
	int i;
	for(i=0;i<n;i++) v=(v<<32)|fbe32(p+4*i);
	return v;
}

/* Collect fixed FDT reservations and bounded dynamic pools. Dynamic pools
 * are placed by the EFI loader inside their declared alloc-ranges. */
char*
armfdtreservations(void *vfdt, uvlong avail, uvlong *base, uvlong *len,
	int cap, int *count, ArmDynamic *dynamic, int dcap, int *ndynamic)
{
	uchar *fdt,*p,*end,*strings,*r,*rend,*v;
	ulong off,offstr,offres,stlen,strlenf,n,noff,pos,tok;
	uvlong a,z;
	char path[256],nodes[16][64],*name;
	ArmDynamic tmp[ArmMaxDynamic];
	int depth,ac,sc,i,j,di,dstack[16],nd;

	*count=0; *ndynamic=0; nd=0;
	memset(tmp,0,sizeof tmp);
	for(i=0;i<sizeof dstack/sizeof dstack[0];i++) dstack[i]=-1;
	if((name=armfdtvalidate(vfdt,avail))!=nil) return name;
	fdt=vfdt; off=fbe32(fdt+8); offstr=fbe32(fdt+12);
	offres=fbe32(fdt+16); strlenf=fbe32(fdt+32); stlen=fbe32(fdt+36);
	strings=fdt+offstr; p=fdt+off; end=p+stlen;
	/* Firmware-provided reservation entries. */
	r=fdt+offres; rend=fdt+off;
	while(r+16<=rend){
		a=((uvlong)fbe32(r)<<32)|fbe32(r+4);
		z=((uvlong)fbe32(r+8)<<32)|fbe32(r+12); r+=16;
		if(a==0 && z==0) break;
		if(*count>=cap || z==0 || a>~(uvlong)0-z)
			return "too many/invalid FDT reserved ranges";
		base[*count]=a; len[*count]=z; (*count)++;
	}
	depth=0; ac=2; sc=1; path[0]=0;
	while(p+4<=end){
		tok=fbe32(p);p+=4;
		if(tok==1){
			for(pos=0;p+pos<end&&p[pos];pos++);
			if(p+pos>=end||depth>=16||pos>=sizeof nodes[0]) return "bad FDT node while reading reservations";
			memmove(nodes[depth],p,pos);nodes[depth][pos]=0;
			if(depth){ if(!fappend(path,"/")||!fappend(path,nodes[depth])) return "FDT path too long"; }
			dstack[depth]=-1;
			if(depth==2 && fncmp(path,"/reserved-memory/",17)){
				if(nd==dcap || nd==ArmMaxDynamic) dstack[depth]=-2;
				else dstack[depth]=nd++;
			}
			depth++;p+=(pos+4)&~3;
		}else if(tok==2){
			if(depth==0)return "unbalanced FDT node";
			di=dstack[depth-1];
			if(di>=0 && tmp[di].size!=0){
				if(tmp[di].nranges==0){
					/* no alloc-ranges means any conventional RAM in the
					 * platform's declared direct map */
				}
				if(*ndynamic>=dcap) return "too many dynamic FDT pools";
				dynamic[(*ndynamic)++]=tmp[di];
			}
			depth--;
			if(depth==0)path[0]=0;
			else{for(pos=strlen(path);pos>0&&path[pos-1]!='/';pos--);if(pos)path[pos-1]=0;}
		}else if(tok==3){
			if(p+8>end)return "truncated FDT reservation property";
			n=fbe32(p);noff=fbe32(p+4);p+=8;
			if(n>(ulong)(end-p)||noff>=strlenf)return "FDT reservation property outside bounds";
			name=(char*)strings+noff;
			for(pos=0;noff+pos<strlenf&&name[pos];pos++);
			if(noff+pos>=strlenf)return "unterminated FDT reservation property name";
			v=p;
			di=depth>0?dstack[depth-1]:-1;
			if(di==-2 && (fcmp(name,"size") || fcmp(name,"alloc-ranges")))
				return "too many dynamic FDT pools";
			if(di>=0 && fcmp(name,"size")){
				if(n!=4*sc || (tmp[di].size=fdtcells(v,sc))==0)
					return "invalid dynamic /reserved-memory size";
			}else if(di>=0 && fcmp(name,"alignment")){
				if(n!=4*sc || (tmp[di].align=fdtcells(v,sc))==0)
					return "invalid dynamic /reserved-memory alignment";
			}else if(di>=0 && fcmp(name,"alloc-ranges")){
				if(n==0 || n%(4*(ac+sc))!=0) return "invalid dynamic /reserved-memory alloc-ranges";
				for(i=0;i<n;i+=4*(ac+sc)){
					if(tmp[di].nranges==ArmMaxAllocRanges) return "too many dynamic FDT alloc-ranges";
					a=fdtcells(v+i,ac); z=fdtcells(v+i+4*ac,sc);
					if(z==0 || a>~(uvlong)0-z) return "invalid dynamic FDT alloc-range";
					j=tmp[di].nranges++;
					tmp[di].rangelo[j]=a; tmp[di].rangelen[j]=z;
				}
			}else if(di>=0 && fcmp(name,"reg") && tmp[di].size!=0)
				return "dynamic /reserved-memory node also has reg";
			if(fcmp(path,"/reserved-memory")&&fcmp(name,"#address-cells")&&n==4){ac=fbe32(v);if(ac<1||ac>2)return "unsupported reserved-memory address cells";}
			else if(fcmp(path,"/reserved-memory")&&fcmp(name,"#size-cells")&&n==4){sc=fbe32(v);if(sc<1||sc>2)return "unsupported reserved-memory size cells";}
			else if(fncmp(path,"/reserved-memory/",17)&&fcmp(name,"reg")){
				if(n==0||n%(4*(ac+sc))!=0)return "invalid /reserved-memory reg property";
				for(i=0;i<n;i+=4*(ac+sc)){
					a=fdtcells(v+i,ac);z=fdtcells(v+i+4*ac,sc);
					if(z==0) continue;
					if(a>~(uvlong)0-z||*count>=cap)return "invalid/too many /reserved-memory ranges";
					base[*count]=a;len[*count]=z;(*count)++;
				}
			}
			p+=(n+3)&~3;
		}else if(tok==4){
		}else if(tok==9){
			return nil;
		}else return "bad FDT token while reading reservations";
	}
	return "FDT ended before reservation scan completed";
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
		if(im->magic!=0x8c47 || im->nseg!=1)
			return "unsupported ARM64 a.out image";
		if(l->platform==ArmQemu && im->entry!=0xffffffffc0100000ULL)
			return "unsupported QEMU ARM64 a.out entry";
		if(l->platform==ArmPi4 && (im->entry!=0xffffffffc1080000ULL
		|| im->seg[0].pa!=0x01080000ULL
		|| im->seg[0].va!=im->entry
		|| l->scratchlo!=0x01000000ULL || l->scratchlen!=0x80000ULL))
			return "unsupported rpi4 headed a.out layout";
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
		if(overlap(s->pa,n,l->fdtlo,l->fdtlen))
			return "ARM64 load segment overlaps firmware FDT";
		for(j=0;j<l->nreserved;j++)
			if(overlap(s->pa,n,l->reservedlo[j],l->reservedlen[j]))
				return "ARM64 load segment overlaps an FDT-reserved range";
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

/* Image bytes and kernel scratch require conventional RAM. Boot-services
 * pages remain EDK2-owned until ExitBootServices and cannot substitute for
 * an explicit scratch reservation. */
static int
armrange(void *map, uvlong bytes, uvlong stride, uvlong pa, uvlong len, int scratch)
{
	EFI_MEMORY_DESCRIPTOR *m;
	uvlong i,end,top;
	int found;

	(void)scratch;
	if(!span(pa,len)) return 0;
	end=pa+len;
	while(pa<end){
		found=0;
		for(i=0;i<bytes;i+=stride){
			m=(EFI_MEMORY_DESCRIPTOR*)((uchar*)map+i);
			top=m->PhysicalStart+m->NumberOfPages*4096;
			if(pa<m->PhysicalStart || pa>=top) continue;
			if((m->Attribute&(1ULL<<63)) || m->Type!=EfiConventionalMemory)
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
	if(l->scratchlen && overlap(l->scratchlo,l->scratchlen,l->fdtlo,l->fdtlen))
		return "ARM64 scratch overlaps firmware FDT";
	for(i=0;i<l->nreserved;i++)
		if(overlap(l->scratchlo,l->scratchlen,l->reservedlo[i],l->reservedlen[i]))
			return "ARM64 scratch overlaps an FDT-reserved range";
	return nil;
}

int
armblob(ArmLayout *l, BootImage *im, void *map, uvlong bytes, uvlong stride,
	uvlong len, uvlong *out)
{
	EFI_MEMORY_DESCRIPTOR *m;
	uvlong i,lo,hi,a,n,cut;
	int j,k,r,hit;

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
			if(overlap(a,n,l->fdtlo,l->fdtlen)){
				if(l->fdtlo<cut) cut=l->fdtlo;
				hit=1;
			}
			for(r=0;r<l->nreserved;r++)
				if(overlap(a,n,l->reservedlo[r],l->reservedlen[r])){
					if(l->reservedlo[r]<cut) cut=l->reservedlo[r];
					hit=1;
				}
			if(!hit){ *out=a; return 1; }
			hi=cut;
		}
	}
	return 0;
}
