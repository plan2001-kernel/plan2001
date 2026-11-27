/*
 * wgkey: WireGuard-avaimet base64-muodossa, kuten wg genkey ja wg pubkey.
 *	ip/wgkey	uusi yksityinen avain (Curve25519, rajattu)
 *	ip/wgkey -p	yksityinen avain syötteestä, julkinen avain tulosteeseen
 * Avain syntyy tällä koneella eikä kulje verkon yli.
 */
#include <u.h>
#include <libc.h>
#include <libsec.h>

enum { Keylen = 32 };

static uchar nine[Keylen] = { 9 };

static void
usage(void)
{
	fprint(2, "usage: ip/wgkey [-p]\n");
	exits("usage");
}

static void
put(uchar *k)
{
	char b[64];

	if(enc64(b, sizeof b, k, Keylen) != 44)
		sysfatal("enc64");
	print("%s\n", b);
}

void
main(int argc, char **argv)
{
	uchar k[Keylen], p[Keylen];
	char b[128], *s;
	int pub, n;

	pub = 0;
	ARGBEGIN{
	case 'p':
		pub = 1;
		break;
	default:
		usage();
	}ARGEND
	if(argc != 0)
		usage();

	if(!pub){
		genrandom(k, Keylen);
		k[0] &= 248;
		k[31] &= 127;
		k[31] |= 64;
		put(k);
		memset(k, 0, Keylen);
		exits(nil);
	}

	n = readn(0, b, sizeof b - 1);
	if(n <= 0)
		sysfatal("no key");
	b[n] = 0;
	if((s = strchr(b, '\n')) != nil)
		*s = 0;
	if(dec64(k, Keylen, b, strlen(b)) != Keylen)
		sysfatal("bad key");
	memset(b, 0, sizeof b);
	if(!x25519(p, k, nine))
		sysfatal("bad key");
	memset(k, 0, Keylen);
	put(p);
	exits(nil);
}
