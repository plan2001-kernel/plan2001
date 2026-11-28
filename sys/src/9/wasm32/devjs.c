#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"../port/error.h"

/*
 * #j/js: sivun (JavaScriptin) ja koneen välinen viestikanava.  Yksi
 * natiivi palvelin (aux/jsio) avaa sen kerrallaan ja palvelee sivun
 * pyyntöjä omassa nimiavaruudessaan; kanava itse ei tunne pyyntöjen
 * sisältöä.  Viestin rajat säilyvät kuten putkessa:
 *
 *	read	yksi sivun viesti kokonaan (odottaa sitä); lukupuskurin on
 *		oltava vähintään viestin kokoinen, muuten Etoosmall
 *	write	yksi viesti sivulle (enintään Maxmsg tavua)
 *
 * Sivu kirjoittaa viestit renkaaseen (platjsring kertoo osoitteen):
 * kukin pituutensa (4 tavua, little-endian) ja tavunsa.  Lähtevät
 * viestit menevät sivulle platjs:llä; platjs(nil, 0) kertoo, että
 * palvelin sulki kanavan (sivu hylkää odottavat pyyntönsä).
 */
enum
{
	Qtop	= 0,
	Qjs,

	Nring	= 512*1024,	/* kahden potenssi: laskurit kiertävät modulo 2^32, indeksi maskataan */
	Maxmsg	= 128*1024,
};

typedef struct Jring Jring;
struct Jring
{
	ulong	r;		/* meidän: luetut tavut */
	ulong	w;		/* sivun: kirjoitetut tavut, kokonaiset viestit */
	uchar	b[Nring];
};

static Jring ring;

static struct
{
	Lock;
	int	open;		/* palvelimella on kanava auki */
	QLock	rl;		/* yksi lukija kerrallaan */
	QLock	wl;		/* viestit sivulle järjestyksessä */
} js;

static Dirtab jstab[] = {
	".",		{Qtop, 0, QTDIR},	0,	0555,
	"js",		{Qjs},			0,	0660,
};

static uchar
ringbyte(ulong i)
{
	return ring.b[i & (Nring-1)];
}

static Chan*
jsattach(char *spec)
{
	return devattach('j', spec);
}

static Walkqid*
jswalk(Chan *c, Chan *nc, char **name, int nname)
{
	return devwalk(c, nc, name, nname, jstab, nelem(jstab), devgen);
}

static int
jsstat(Chan *c, uchar *db, int n)
{
	return devstat(c, db, n, jstab, nelem(jstab), devgen);
}

static Chan*
jsopen(Chan *c, int omode)
{
	c = devopen(c, omode, jstab, nelem(jstab), devgen);
	if((ulong)c->qid.path == Qjs){
		lock(&js);
		if(js.open){
			unlock(&js);
			c->flag &= ~COPEN;
			error(Einuse);
		}
		js.open = 1;
		unlock(&js);
		/* uusi palvelin: sivun vanhat, lukemattomat pyynnöt jäävät pois */
		ring.r = ring.w;
		platjsring(&ring);
	}
	return c;
}

static void
jsclose(Chan *c)
{
	if((ulong)c->qid.path == Qjs && (c->flag & COPEN) != 0){
		lock(&js);
		js.open = 0;
		unlock(&js);
		platjs(nil, 0);
	}
}

static long
jsread(Chan *c, void *a, long n, vlong)
{
	ulong w, len, i;
	uchar *p;

	switch((ulong)c->qid.path){
	case Qtop:
		return devdirread(c, a, n, jstab, nelem(jstab), devgen);
	case Qjs:
		break;
	default:
		error(Egreg);
	}
	eqlock(&js.rl);
	if(waserror()){
		qunlock(&js.rl);
		nexterror();
	}
	/* sivun herätys (Atomics.notify) tai 100 ms: muistiinpano keskeyttää odotuksen */
	while((w = ring.w) - ring.r < 4){
		if(up->notepending)
			error(Eintr);
		platwait((long*)&ring.w, w, 100);
	}
	len = ringbyte(ring.r) | ringbyte(ring.r+1)<<8 | ringbyte(ring.r+2)<<16 | (ulong)ringbyte(ring.r+3)<<24;
	if(len > Maxmsg || len > w - ring.r - 4){	/* ei sivun: ei koskaan */
		print("#j: bad message in the ring\n");
		ring.r = w;
		error(Ebadarg);
	}
	if(len > n)
		error(Etoosmall);
	p = a;
	for(i = 0; i < len; i++)
		p[i] = ringbyte(ring.r+4+i);
	coherence();
	ring.r += 4+len;
	poperror();
	qunlock(&js.rl);
	return len;
}

static long
jswrite(Chan *c, void *a, long n, vlong)
{
	if((ulong)c->qid.path != Qjs)
		error(Eperm);
	if(n <= 0 || n > Maxmsg)
		error(Etoobig);
	eqlock(&js.wl);
	platjs(a, n);
	qunlock(&js.wl);
	return n;
}

Dev jsdevtab = {
	'j',
	"js",

	devreset,
	devinit,
	devshutdown,
	jsattach,
	jswalk,
	jsstat,
	jsopen,
	devcreate,
	jsclose,
	jsread,
	devbread,
	jswrite,
	devbwrite,
	devremove,
	devwstat,
};
