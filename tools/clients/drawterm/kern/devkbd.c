#include	"u.h"
#include	"lib.h"
#include	"dat.h"
#include	"fns.h"
#include	"error.h"

static Queue*	keyq;
static int kbdinuse;

void
kbdkey(Rune r, int down)
{
	char buf[2+UTFmax];

	if(r == 0)
		return;

	if(!kbdinuse || keyq == nil){
		if(down)
			kbdputc(kbdq, r);	/* /dev/cons */
		return;
	}

	memset(buf, 0, sizeof buf);
	buf[0] = down ? 'r' : 'R';
#ifdef NINEPTERM
	/* tty's kproc may sleep; graphical GUI callbacks must not. */
	qwrite(keyq, buf, 2+runetochar(buf+1, &r));
#else
	qproduce(keyq, buf, 2+runetochar(buf+1, &r));
#endif
}

enum{
	Qdir,
	Qkbd,
};

static Dirtab kbddir[]={
	".",	{Qdir, 0, QTDIR},	0,		DMDIR|0555,
	"kbd",		{Qkbd},		0,		0444,
};

static void
kbdinit(void)
{
#ifdef NINEPTERM
	keyq = qopen(4*1024, Qmsg, 0, 0);
#else
	keyq = qopen(4*1024, Qcoalesce, 0, 0);
#endif
	if(keyq == nil)
		panic("kbdinit");
#ifndef NINEPTERM
	qnoblock(keyq, 1);
#endif
}

static Chan*
kbdattach(char *spec)
{
	return devattach('b', spec);
}

static Walkqid*
kbdwalk(Chan *c, Chan *nc, char **name, int nname)
{
	return devwalk(c, nc, name,nname, kbddir, nelem(kbddir), devgen);
}

static int
kbdstat(Chan *c, uchar *dp, int n)
{
	return devstat(c, dp, n, kbddir, nelem(kbddir), devgen);
}

static Chan*
kbdopen(Chan *c, int omode)
{
	c = devopen(c, omode, kbddir, nelem(kbddir), devgen);
	switch((ulong)c->qid.path){
	case Qkbd:
		if(tas(&kbdinuse) != 0){
			c->flag &= ~COPEN;
			error(Einuse);
		}
		break;
	}
	return c;
}

static void
kbdclose(Chan *c)
{
	switch((ulong)c->qid.path){
	case Qkbd:
		if(c->flag&COPEN)
			kbdinuse = 0;
		break;
	}
}

static long
kbdread(Chan *c, void *buf, long n, vlong off)
{
	USED(off);

	if(n <= 0)
		return n;
	switch((ulong)c->qid.path){
	case Qdir:
		return devdirread(c, buf, n, kbddir, nelem(kbddir), devgen);
	case Qkbd:
#ifdef NINEPTERM
		{
			long m, total;

			/* kbdfs expects complete NUL-terminated events per read. */
			if(n < UTFmax+2)
				error(Eshort);
			total = qread(keyq, buf, UTFmax+2);
			while(n-total >= UTFmax+2 && qcanread(keyq)){
				m = qread(keyq, (char*)buf+total, UTFmax+2);
				if(m <= 0)
					break;
				total += m;
			}
			return total;
		}
#else
		return qread(keyq, buf, n);
#endif
	default:
		print("kbdread 0x%llux\n", c->qid.path);
		error(Egreg);
	}
	return -1;		/* never reached */
}

static long
kbdwrite(Chan *c, void *va, long n, vlong off)
{
	USED(c);
	USED(va);
	USED(n);
	USED(off);
	error(Eperm);
	return -1;		/* never reached */
}

Dev kbddevtab = {
	'b',
	"kbd",

	devreset,
	kbdinit,
	devshutdown,
	kbdattach,
	kbdwalk,
	kbdstat,
	kbdopen,
	devcreate,
	kbdclose,
	kbdread,
	devbread,
	kbdwrite,
	devbwrite,
	devremove,
	devwstat,
};
