#ifndef PLAN2001_EFI_IMAGE_H
#define PLAN2001_EFI_IMAGE_H
/* Loader-internal image plan, not a change to BootInfo v1. */
enum { ImageAout=1, ImageElf, ImageExec=1, ImageMaxSeg=16 };
typedef struct BootSegment BootSegment;
typedef struct BootImage BootImage;
struct BootSegment {
	uvlong pa, va, filesz, memsz, allocsz;
	u32int flags;
};
struct BootImage {
	int format, magic, nseg;
	uvlong entry, entrypa;
	BootSegment seg[ImageMaxSeg];
};
#endif
