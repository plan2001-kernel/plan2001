/*
 * virtio ethernet driver implementing the legacy interface:
 * http://docs.oasis-open.org/virtio/virtio/v1.0/virtio-v1.0.html
 */
#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "io.h"
#include "../port/pci.h"
#include "ureg.h"
#include "../port/error.h"

#include "../port/sd.h"

typedef struct Vring Vring;
typedef struct Vdesc Vdesc;
typedef struct Vused Vused;
typedef struct Vqueue Vqueue;
typedef struct Vdev Vdev;

typedef struct ScsiCfg ScsiCfg;
typedef struct Vlun Vlun;

/* device types */
enum {
	TypBlk	= 2,
	TypSCSI	= 8,
};

/* status flags */
enum {
	Acknowledge = 1,
	Driver = 2,
	DriverOk = 4,
	Failed = 0x80,
};

/* virtio ports */
enum {
	Devfeat = 0,
	Drvfeat = 4,
	Qaddr = 8,
	Qsize = 12,
	Qselect = 14,
	Qnotify = 16,
	Status = 18,
	Isr = 19,

	Devspec = 20,
};

/* descriptor flags */
enum {
	Next = 1,
	Write = 2,
	Indirect = 4,
};

/* struct sizes */
enum {
	VringSize = 4,
};	

struct Vring
{
	u16int	flags;
	u16int	idx;
};

struct Vdesc
{
	u64int	addr;
	u32int	len;
	u16int	flags;
	u16int	next;
};

struct Vused
{
	u32int	id;
	u32int	len;
};

struct Vqueue
{
	Lock;

	Vdev	*dev;
	int	idx;

	int	size;

	int	free;
	int	nfree;

	Vdesc	*desc;

	Vring	*avail;
	u16int	*availent;
	u16int	*availevent;

	Vring	*used;
	Vused	*usedent;
	u16int	*usedevent;
	u16int	lastused;

	void	*rock[];
};

struct Vdev
{
	int	typ;

	Pcidev	*pci;

	ulong	port;
	ulong	feat;

	int	nqueue;
	Vqueue	*queue[16];

	void	*cfg;	/* device specific config (for scsi) */
	Vlun	*lunmap;	/* scsi: unit number to target and lun */
	int	scanned;

	Vdev	*next;
};

enum {
	CDBSIZE		= 32,
	SENSESIZE	= 96,
};

/* virtio-scsi request response */
enum {
	VSok		= 0,
	VSoverrun,
	VSaborted,
	VSbadtarget,
	VSreset,
	VSbusy,
};

enum {
	Maxunit		= 256,		/* units of a controller (devsd's UnitLOG) */
	Rlunsize	= 8+8*Maxunit,	/* REPORT LUNS parameter data */
};

/*
 * A logical unit: target and the two bytes of its single level
 * LUN structure, as REPORT LUNS gives them.
 */
struct Vlun
{
	u8int	present;
	u8int	target;
	u8int	lun[2];
};

struct ScsiCfg
{
	u32int	num_queues;
	u32int	seg_max;
	u32int	max_sectors;
	u32int	cmd_per_lun;
	u32int	event_info_size;
	u32int	sense_size;
	u32int	cdb_size;
	u16int	max_channel;
	u16int	max_target;
	u32int	max_lun;
};

static Vqueue*
mkvqueue(int size)
{
	Vqueue *q;
	uchar *p;
	int i;

	q = malloc(sizeof(*q) + sizeof(void*)*size);
	p = mallocalign(
		PGROUND(sizeof(Vdesc)*size + 
			VringSize + 
			sizeof(u16int)*size + 
			sizeof(u16int)) +
		PGROUND(VringSize + 
			sizeof(Vused)*size + 
			sizeof(u16int)), 
		BY2PG, 0, 0);
	if(p == nil || q == nil){
		print("virtio: no memory for Vqueue\n");
		free(p);
		free(q);
		return nil;
	}

	q->desc = (void*)p;
	p += sizeof(Vdesc)*size;
	q->avail = (void*)p;
	p += VringSize;
	q->availent = (void*)p;
	p += sizeof(u16int)*size;
	q->availevent = (void*)p;
	p += sizeof(u16int);

	p = (uchar*)PGROUND((uintptr)p);
	q->used = (void*)p;
	p += VringSize;
	q->usedent = (void*)p;
	p += sizeof(Vused)*size;
	q->usedevent = (void*)p;

	q->free = -1;
	q->nfree = q->size = size;
	for(i=0; i<size; i++){
		q->desc[i].next = q->free;
		q->free = i;
	}

	return q;
}

static Vdev*
viopnpdevs(int typ)
{
	Vdev *vd, *h, *t;
	Vqueue *q;
	Pcidev *p;
	int n, i;

	h = t = nil;
	for(p = nil; p = pcimatch(p, 0x1AF4, 0);){
		if((p->did < 0x1000) || (p->did > 0x103F))
			continue;
		if(p->rid != 0)
			continue;
		if((p->mem[0].bar & 1) == 0)
			continue;
		if(pcicfgr16(p, 0x2E) != typ)
			continue;
		if((vd = malloc(sizeof(*vd))) == nil){
			print("virtio: no memory for Vdev\n");
			break;
		}
		vd->port = p->mem[0].bar & ~3;
		if(ioalloc(vd->port, p->mem[0].size, 0, "virtio") < 0){
			print("virtio: port %lux in use\n", vd->port);
			free(vd);
			continue;
		}
		vd->typ = typ;
		vd->pci = p;
		pcienable(p);

		/* reset */
		outb(vd->port+Status, 0);

		vd->feat = inl(vd->port+Devfeat);
		outb(vd->port+Status, Acknowledge|Driver);
		for(i=0; i<nelem(vd->queue); i++){
			outs(vd->port+Qselect, i);
			n = ins(vd->port+Qsize);
			if(n == 0 || (n & (n-1)) != 0)
				break;
			if((q = mkvqueue(n)) == nil)
				break;
			q->dev = vd;
			q->idx = i;
			vd->queue[i] = q;
			coherence();
			outl(vd->port+Qaddr, PADDR(vd->queue[i]->desc)/BY2PG);
		}
		vd->nqueue = i;
	
		if(h == nil)
			h = vd;
		else
			t->next = vd;
		t = vd;
	}

	return h;
}

struct Rock {
	int done;
	Rendez *sleep;
};

static void
vqinterrupt(Vqueue *q)
{
	int id, free, m;
	struct Rock *r;
	Rendez *z;

	m = q->size-1;

	ilock(q);
	while((q->lastused ^ q->used->idx) & m){
		id = q->usedent[q->lastused++ & m].id;
		if(r = q->rock[id]){
			q->rock[id] = nil;
			z = r->sleep;
			r->done = 1;	/* hands off */
			if(z != nil)
				wakeup(z);
		}
		do {
			free = id;
			id = q->desc[free].next;
			q->desc[free].next = q->free;
			q->free = free;
			q->nfree++;
		} while(q->desc[free].flags & Next);
	}
	iunlock(q);
}

static void
viointerrupt(Ureg *, void *arg)
{
	Vdev *vd = arg;

	if(inb(vd->port+Isr) & 1)
		vqinterrupt(vd->queue[vd->typ == TypSCSI ? 2 : 0]);
}

static int
viodone(void *arg)
{
	return ((struct Rock*)arg)->done;
}

static void
vqio(Vqueue *q, int head)
{
	struct Rock rock;

	rock.done = 0;
	rock.sleep = &up->sleep;
	q->rock[head] = &rock;
	q->availent[q->avail->idx & (q->size-1)] = head;
	coherence();
	q->avail->idx++;
	iunlock(q);
	if((q->used->flags & 1) == 0)
		outs(q->dev->port+Qnotify, q->idx);
	while(!rock.done){
		while(waserror())
			;
		tsleep(rock.sleep, viodone, &rock, 1000);
		poperror();

		if(!rock.done)
			vqinterrupt(q);
	}
}

static int
vioblkreq(Vdev *vd, int typ, void *a, long count, long secsize, uvlong lba)
{
	int need, free, head;
	Vqueue *q;
	Vdesc *d;

	u8int status;
	struct Vioblkreqhdr {
		u32int	typ;
		u32int	prio;
		u64int	lba;
	} req;

	need = 2;
	if(a != nil)
		need = 3;

	status = -1;
	req.typ = typ;
	req.prio = 0;
	req.lba = lba;

	q = vd->queue[0];
	ilock(q);
	while(q->nfree < need){
		iunlock(q);

		if(!waserror())
			tsleep(&up->sleep, return0, 0, 500);
		poperror();

		ilock(q);
	}

	head = free = q->free;

	d = &q->desc[free]; free = d->next;
	d->addr = PADDR(&req);
	d->len = sizeof(req);
	d->flags = Next;

	if(a != nil){
		d = &q->desc[free]; free = d->next;
		d->addr = PADDR(a);
		d->len = secsize*count;
		d->flags = typ ? Next : (Write|Next);
	}

	d = &q->desc[free]; free = d->next;
	d->addr = PADDR(&status);
	d->len = sizeof(status);
	d->flags = Write;

	q->free = free;
	q->nfree -= need;

	/* queue io, unlock and wait for completion */
	vqio(q, head);

	return status;
}

/*
 * One command to the logical unit lun (single level LUN structure)
 * of target t; resp gets the device's response: sense_len, residual,
 * status qualifier, status, response and sense.
 */
static void
vioscsiio(Vdev *vd, int t, u8int *lun, u8int *cmd, int clen, void *data, long dlen, int write, u8int *resp)
{
	u8int req[8+8+3+CDBSIZE];
	u64int tag;
	int free, head;
	Vqueue *q;
	Vdesc *d;
	ScsiCfg *cfg;

	cfg = vd->cfg;

	memset(resp, 0, 4+4+2+2+SENSESIZE);
	memset(req, 0, sizeof(req));
	req[0] = 1;
	req[1] = t;
	req[2] = lun[0];
	req[3] = lun[1];
	/* the tag, any value: byte copy, req[8] need not be 8-byte aligned */
	tag = (uintptr)resp;
	memmove(&req[8], &tag, sizeof(tag));

	memmove(&req[8+8+3], cmd, clen);

	q = vd->queue[2];
	ilock(q);
	while(q->nfree < 3){
		iunlock(q);

		if(!waserror())
			tsleep(&up->sleep, return0, 0, 500);
		poperror();

		ilock(q);
	}

	head = free = q->free;

	d = &q->desc[free]; free = d->next;
	d->addr = PADDR(req);
	d->len = 8+8+3+cfg->cdb_size;
	d->flags = Next;

	if(write && dlen > 0){
		d = &q->desc[free]; free = d->next;
		d->addr = PADDR(data);
		d->len = dlen;
		d->flags = Next;
	}

	d = &q->desc[free]; free = d->next;
	d->addr = PADDR(resp);
	d->len = 4+4+2+2+cfg->sense_size;
	d->flags = Write;

	if(!write && dlen > 0){
		d->flags |= Next;

		d = &q->desc[free]; free = d->next;
		d->addr = PADDR(data);
		d->len = dlen;
		d->flags = Write;
	}
	
	q->free = free;
	q->nfree -= 2 + (dlen > 0);

	/* queue io, unlock and wait for completion */
	vqio(q, head);
}

static int
vioscsireq(SDreq *r)
{
	u8int resp[4+4+2+2+SENSESIZE];
	u32int len;
	Vdev *vd;
	Vlun *l;
	SDunit *u;

	u = r->unit;
	vd = u->dev->ctlr;

	/*
	 * The unit's own target and lun (vioscsiscan), not r->lun:
	 * that is the SCSI-2 lun of the command block, always 0 here.
	 */
	l = &vd->lunmap[u->subno];
	if(!l->present)
		return sdsetsense(r, SDcheck, 0x05, 0x25, 0);
	vioscsiio(vd, l->target, l->lun, r->cmd, r->clen, r->data, r->dlen, r->write, resp);

	/* response+status */
	r->status = resp[10];
	if(resp[11] != 0)
		r->status = SDcheck;

	/* sense_len */
	len = *((u32int*)&resp[0]);
	if(len > 0){
		if(len > sizeof(r->sense))
			len = sizeof(r->sense);
		memmove(r->sense, &resp[4+4+2+2], len);
		r->flags |= SDvalidsense;
	}

	/* data residue */
	len = *((u32int*)&resp[4]);
	if(len > r->dlen)
		r->rlen = 0;
	else
		r->rlen = r->dlen - len;

	return r->status;

}

static int
vlunno(Vlun *l)
{
	return (l->lun[0]&0x3F)<<8 | l->lun[1];
}

/*
 * The logical units of target t, from REPORT LUNS to its lun 0,
 * which a target answers whether or not it has a logical unit 0
 * (SPC-3 4.6.1).  Returns how many it put in luns, -1 when there
 * is no such target, -2 when it does not report its logical units.
 */
static int
vioscsiluns(Vdev *vd, int t, uchar *buf, Vlun *luns, int max)
{
	static u8int lun0[2];
	u8int cmd[12], resp[4+4+2+2+SENSESIZE];
	uchar *e;
	long rlen;
	int i, n, try;

	for(try = 0;; try++){
		if(try == 3)
			return -2;
		memset(cmd, 0, sizeof(cmd));
		cmd[0] = 0xA0;
		cmd[6] = Rlunsize>>24;
		cmd[7] = Rlunsize>>16;
		cmd[8] = Rlunsize>>8;
		cmd[9] = Rlunsize;
		memset(buf, 0, Rlunsize);
		vioscsiio(vd, t, lun0, cmd, sizeof(cmd), buf, Rlunsize, 0, resp);
		if(resp[11] == VSbadtarget)
			return -1;
		if(resp[11] == VSbusy || resp[11] == VSreset || resp[11] == VSaborted)
			continue;
		if(resp[11] != VSok)
			return -2;
		if(resp[10] == SDok)
			break;
		if(resp[10] != SDcheck && resp[10] != SDbusy)
			return -2;
	}

	rlen = *((u32int*)&resp[4]);
	if(rlen > Rlunsize)
		rlen = Rlunsize;
	rlen = Rlunsize - rlen;
	if(rlen < 8)
		return -2;
	n = buf[0]<<24 | buf[1]<<16 | buf[2]<<8 | buf[3];
	if(n > rlen-8)
		n = rlen-8;
	i = 0;
	for(e = buf+8; e+8 <= buf+8+n && i < max; e += 8){
		/* single level: peripheral device (bus 0) or flat space addressing */
		if(e[2] | e[3] | e[4] | e[5] | e[6] | e[7])
			continue;
		if((e[0]>>6) > 1 || ((e[0]>>6) == 0 && e[0] != 0))
			continue;
		luns[i].present = 1;
		luns[i].target = t;
		luns[i].lun[0] = e[0];
		luns[i].lun[1] = e[1];
		i++;
	}
	return i;
}

/*
 * Whether INQUIRY finds a device at the logical unit: a target
 * reports its lun 0 even without one there (peripheral qualifier
 * 1 and device type 1Fh, or qualifier 3).
 */
static int
vioscsipresent(Vdev *vd, Vlun *l, uchar *buf)
{
	u8int cmd[6], resp[4+4+2+2+SENSESIZE];
	int try;

	for(try = 0; try < 3; try++){
		memset(cmd, 0, sizeof(cmd));
		cmd[0] = 0x12;
		cmd[4] = 36;
		memset(buf, 0, 36);
		vioscsiio(vd, l->target, l->lun, cmd, sizeof(cmd), buf, 36, 0, resp);
		if(resp[11] == VSbusy || resp[11] == VSreset || resp[11] == VSaborted)
			continue;
		if(resp[11] != VSok)
			return 0;
		if(resp[10] == SDok)
			return (buf[0]>>5) == 0 || ((buf[0]>>5) == 1 && (buf[0]&0x1F) != 0x1F);
		if(resp[10] != SDcheck && resp[10] != SDbusy)
			return 0;
	}
	return 0;
}

/*
 * Logical unit 0 of target t, when there is a device, is unit t, as
 * it has always been; the other logical units, in target and lun
 * order, take the unit numbers no target's logical unit 0 has.
 */
static void
vioscsiscan(SDev *sd, int maxtarget)
{
	Vdev *vd;
	Vlun *map, *luns, *x, l;
	uchar *buf;
	int t, i, j, n, nx, s;

	vd = sd->ctlr;
	map = vd->lunmap;
	buf = sdmalloc(Rlunsize);
	luns = malloc(Maxunit*sizeof(Vlun));
	x = malloc(Maxunit*sizeof(Vlun));
	if(buf == nil || luns == nil || x == nil){
		print("%s: no memory for the scsi scan\n", sd->name);
		goto Out;
	}
	if(maxtarget >= Maxunit)
		maxtarget = Maxunit-1;
	nx = 0;
	for(t = 0; t <= maxtarget; t++){
		n = vioscsiluns(vd, t, buf, luns, Maxunit);
		if(n == -1)
			continue;
		if(n == -2){
			/* no REPORT LUNS: logical unit 0 only */
			memset(luns, 0, sizeof(Vlun));
			luns[0].present = 1;
			luns[0].target = t;
			n = 1;
		}
		for(i = 0; i < n; i++){
			if(!vioscsipresent(vd, &luns[i], buf))
				continue;
			if(vlunno(&luns[i]) == 0)
				map[t] = luns[i];
			else if(nx < Maxunit)
				x[nx++] = luns[i];
		}
	}

	/* insertion sort by target and lun */
	for(i = 1; i < nx; i++){
		l = x[i];
		for(j = i; j > 0; j--){
			if(x[j-1].target < l.target
			|| (x[j-1].target == l.target && vlunno(&x[j-1]) <= vlunno(&l)))
				break;
			x[j] = x[j-1];
		}
		x[j] = l;
	}

	s = 0;
	for(i = 0; i < nx; i++){
		if(i > 0 && x[i].target == x[i-1].target && vlunno(&x[i]) == vlunno(&x[i-1]))
			continue;
		while(s < Maxunit && map[s].present)
			s++;
		if(s == Maxunit){
			print("%s: no unit for target %d lun %d\n", sd->name, x[i].target, vlunno(&x[i]));
			continue;
		}
		map[s] = x[i];
		print("%s%x: target %d lun %d\n", sd->name, s, x[i].target, vlunno(&x[i]));
	}
Out:
	sdfree(buf);
	free(luns);
	free(x);
}

static long
viobio(SDunit *u, int lun, int write, void *a, long count, uvlong lba)
{
	long ss, cc, max, ret;
	Vdev *vd;

	vd = u->dev->ctlr;
	if(vd->typ == TypSCSI)
		return scsibio(u, lun, write, a, count, lba);

	max = 32;
	ss = u->secsize;
	ret = 0;
	while(count > 0){
		if((cc = count) > max)
			cc = max;
		if(vioblkreq(vd, write != 0, (uchar*)a + ret, cc, ss, lba) != 0)
			error(Eio);
		ret += cc*ss;
		count -= cc;
		lba += cc;
	}
	return ret;
}

static int
viorio(SDreq *r)
{
	int i, count, rw;
	uvlong lba;
	SDunit *u;
	Vdev *vd;

	u = r->unit;
	vd = u->dev->ctlr;
	if(vd->typ == TypSCSI)
		return vioscsireq(r);
	if(r->cmd[0] == 0x35 || r->cmd[0] == 0x91){
		if(vioblkreq(vd, 4, nil, 0, 0, 0) != 0)
			return sdsetsense(r, SDcheck, 3, 0xc, 2);
		return sdsetsense(r, SDok, 0, 0, 0);
	}
	if((i = sdfakescsi(r)) != SDnostatus)
		return r->status = i;
	if((i = sdfakescsirw(r, &lba, &count, &rw)) != SDnostatus)
		return i;
	r->rlen = viobio(u, r->lun, rw == SDwrite, r->data, count, lba);
	return r->status = SDok;
}

static int
vioonline(SDunit *u)
{
	uvlong cap;
	Vdev *vd;

	vd = u->dev->ctlr;
	if(vd->typ == TypSCSI)
		return scsionline(u);

	cap = inl(vd->port+Devspec+4);
	cap <<= 32;
	cap |= inl(vd->port+Devspec);
	if(u->sectors != cap){
		u->sectors = cap;
		u->secsize = 512;
		return 2;
	}
	return 1;
}

static int
vioverify(SDunit *u)
{
	Vdev *vd;

	vd = u->dev->ctlr;
	if(vd->typ == TypSCSI){
		if(!vd->lunmap[u->subno].present)
			return 0;
		return scsiverify(u);
	}

	return 1;
}

SDifc sdvirtioifc;

static int
vioenable(SDev *sd)
{
	char name[32];
	Vdev *vd;

	vd = sd->ctlr;
	pcisetbme(vd->pci);
	snprint(name, sizeof(name), "%s (%s)", sd->name, sd->ifc->name);
	intrenable(vd->pci->intl, viointerrupt, vd, vd->pci->tbdf, name);
	outb(vd->port+Status, inb(vd->port+Status) | DriverOk);
	if(vd->typ == TypSCSI && !vd->scanned){
		vd->scanned = 1;
		vioscsiscan(sd, ((ScsiCfg*)vd->cfg)->max_target);
	}
	return 1;
}

static int
viodisable(SDev *sd)
{
	char name[32];
	Vdev *vd;

	vd = sd->ctlr;
	snprint(name, sizeof(name), "%s (%s)", sd->name, sd->ifc->name);
	intrdisable(vd->pci->intl, viointerrupt, vd, vd->pci->tbdf, name);
	pciclrbme(vd->pci);
	return 1;
}

static SDev*
viopnp(void)
{
	SDev *s, *h, *t;
	Vdev *vd;
	int id;

	h = t = nil;

	id = 'F';
	for(vd =  viopnpdevs(TypBlk); vd; vd = vd->next){
		if(vd->nqueue == 0)
			continue;

		if((s = malloc(sizeof(*s))) == nil)
			break;
		s->ctlr = vd;
		s->idno = id++;
		s->ifc = &sdvirtioifc;
		s->nunit = 1;
		if(h)
			t->next = s;
		else
			h = s;
		t = s;
	}

	id = '0';
	for(vd = viopnpdevs(TypSCSI); vd; vd = vd->next){
		ScsiCfg *cfg;

		if(vd->nqueue < 3)
			continue;

		if((cfg = malloc(sizeof(*cfg))) == nil)
			break;
		cfg->num_queues = inl(vd->port+Devspec+4*0);
		cfg->seg_max = inl(vd->port+Devspec+4*1);
		cfg->max_sectors = inl(vd->port+Devspec+4*2);
		cfg->cmd_per_lun = inl(vd->port+Devspec+4*3);
		cfg->event_info_size = inl(vd->port+Devspec+4*4);
		cfg->sense_size = inl(vd->port+Devspec+4*5);
		cfg->cdb_size = inl(vd->port+Devspec+4*6);
		cfg->max_channel = ins(vd->port+Devspec+4*7);
		cfg->max_target = ins(vd->port+Devspec+4*7+2);
		cfg->max_lun = inl(vd->port+Devspec+4*8);

		/* max_target is the highest target id: 0 is one target */
		if((cfg->cdb_size > CDBSIZE) || (cfg->sense_size > SENSESIZE)){
			print("sdvirtio: cdb %ud or sense size %ud too big\n",
				cfg->cdb_size, cfg->sense_size);
			free(cfg);
			continue;
		}
		if((vd->lunmap = malloc(Maxunit*sizeof(Vlun))) == nil){
			free(cfg);
			break;
		}
		vd->cfg = cfg;

		if((s = malloc(sizeof(*s))) == nil)
			break;
		s->ctlr = vd;
		s->idno = id++;
		s->ifc = &sdvirtioifc;
		s->nunit = Maxunit;
		if(h)
			t->next = s;
		else
			h = s;
		t = s;
	}

	return h;
}

SDifc sdvirtioifc = {
	"virtio",			/* name */

	viopnp,				/* pnp */
	vioenable,			/* enable */
	viodisable,			/* disable */

	vioverify,			/* verify */
	vioonline,			/* online */
	viorio,				/* rio */
	nil,				/* rctl */
	nil,				/* wctl */

	viobio,				/* bio */
	nil,				/* probe */
	nil,				/* clear */
	nil,				/* rtopctl */
	nil,				/* wtopctl */
};
