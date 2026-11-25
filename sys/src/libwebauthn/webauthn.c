/*
 * WebAuthn's checks on the relying party's side (webauthn.h): a
 * browser's clientDataJSON, the authenticator's data with its attested
 * credential (create), and an assertion's ES256 signature (get).
 */
#include <u.h>
#include <libc.h>
#include <mp.h>
#include <libsec.h>
#include <json.h>
#include <webauthn.h>

static char b64uset[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

int
wab64uok(char *s, int min, int max)
{
	int n;

	n = strlen(s);
	return n >= min && n <= max && strspn(s, b64uset) == n;
}

int
wab64uenc(char *s, int ns, uchar *p, int n)
{
	char *t;

	if(enc64(s, ns, p, n) < 0)
		return -1;
	for(t = s; *t; t++)
		if(*t == '+')
			*t = '-';
		else if(*t == '/')
			*t = '_';
		else if(*t == '='){
			*t = 0;
			break;
		}
	return t - s;
}

int
wab64udec(char *s, uchar *p, int n)
{
	char *t, *u;
	int m;

	m = strlen(s);
	if(m == 0 || m % 4 == 1 || strspn(s, b64uset) != m)
		return -1;
	if((t = malloc(m + 4)) == nil)
		return -1;
	strcpy(t, s);
	for(u = t; *u; u++)
		if(*u == '-')
			*u = '+';
		else if(*u == '_')
			*u = '/';
	while(m % 4)
		t[m++] = '=';
	t[m] = 0;
	m = dec64(p, n, t, m);
	free(t);
	return m;
}

/* a challenge: WAchal random bytes, base64url */
void
wachallenge(char *s, int ns)
{
	uchar b[WAchal];

	genrandom(b, sizeof b);
	wab64uenc(s, ns, b, sizeof b);
	memset(b, 0, sizeof b);
}

static char*
str(JSON *j, char *key)
{
	JSON *v;

	if((v = jsonbyname(j, key)) == nil || v->t != JSONString)
		return nil;
	return v->s;
}

/*
 * clientDataJSON (n bytes, not NUL-terminated): what was asked (type:
 * webauthn.create or webauthn.get), this challenge, from one of the
 * origins, not from a frame of another site (crossOrigin, topOrigin).
 */
char*
waclient(char *json, int n, char *type, char *chal, char **origins, int norigins)
{
	char *s, *why, *v;
	JSON *j, *x;
	int i;

	if(n <= 0 || n > 16*1024 || memchr(json, 0, n) != nil)
		return "bad client data";
	if((s = malloc(n+1)) == nil)
		return "no memory";
	memmove(s, json, n);
	s[n] = 0;
	j = jsonparse(s);
	free(s);
	if(j == nil || j->t != JSONObject){
		jsonfree(j);
		return "client data not JSON";
	}
	if((v = str(j, "type")) == nil || strcmp(v, type) != 0)
		why = "not what was asked";
	else if((v = str(j, "challenge")) == nil || strlen(v) != strlen(chal) || tsmemcmp(v, chal, strlen(chal)) != 0)
		why = "not this challenge";
	else if((x = jsonbyname(j, "crossOrigin")) != nil && (x->t != JSONBool || x->n != 0))
		why = "cross-origin";
	else if(jsonbyname(j, "topOrigin") != nil)
		why = "cross-origin";
	else if((v = str(j, "origin")) == nil)
		why = "no origin";
	else{
		why = "not this site's origin";
		for(i = 0; i < norigins; i++)
			if(strcmp(v, origins[i]) == 0)
				why = nil;
	}
	jsonfree(j);
	return why;
}

/* CBOR, as much as a COSE key is: integers, byte strings, a map */
typedef struct Cbor Cbor;
struct Cbor
{
	uchar	*p, *e;
};

static int
cborhead(Cbor *c, int *major, uvlong *v)
{
	int ai, i, n;

	if(c->p >= c->e)
		return -1;
	*major = *c->p >> 5;
	ai = *c->p++ & 0x1f;
	if(ai < 24){
		*v = ai;
		return 0;
	}
	if(ai > 27)
		return -1;	/* indefinite lengths and the reserved: not a key's */
	n = 1 << (ai-24);
	if(c->e - c->p < n)
		return -1;
	*v = 0;
	for(i = 0; i < n; i++)
		*v = *v<<8 | *c->p++;
	return 0;
}

static int
cborint(Cbor *c, vlong *x)
{
	int m;
	uvlong v;

	if(cborhead(c, &m, &v) < 0 || v > 0x7fffffff)
		return -1;
	if(m == 0)
		*x = v;
	else if(m == 1)
		*x = -1 - (vlong)v;
	else
		return -1;
	return 0;
}

/*
 * a COSE_Key: kty 2 (EC2), alg -7 (ES256), crv 1 (P-256), x and y of
 * 32 bytes each; nothing else
 */
static char*
cosekey(Cbor *c, uchar pub[WApub])
{
	int m, i, have;
	uvlong n, len;
	vlong k, v;

	if(cborhead(c, &m, &n) < 0 || m != 5 || n < 5 || n > 16)
		return "public key not a COSE key";
	have = 0;
	for(i = 0; i < n; i++){
		if(cborint(c, &k) < 0)
			return "public key not a COSE key";
		if(k == -2 || k == -3){
			if(cborhead(c, &m, &len) < 0 || m != 2 || len != 32 || c->e - c->p < 32)
				return "public key not P-256";
			memmove(pub + (k == -2 ? 1 : 33), c->p, 32);
			c->p += 32;
			have |= k == -2 ? 8 : 16;
			continue;
		}
		if(cborint(c, &v) < 0)
			return "public key not ES256";
		switch((int)k){
		case 1:
			if(v != 2)
				return "public key not EC2";
			have |= 1;
			break;
		case 3:
			if(v != -7)
				return "public key not ES256";
			have |= 2;
			break;
		case -1:
			if(v != 1)
				return "public key not P-256";
			have |= 4;
			break;
		default:
			return "public key not ES256";
		}
	}
	if(have != 31)
		return "public key not ES256";
	pub[0] = 4;
	return nil;
}

/*
 * authenticatorData: rpIdHash[32] flags[1] signCount[4], then with AT the
 * attested credential data (aaguid[16] idlen[2] id COSE_Key); with ED
 * extensions (not read) to the end
 */
char*
waparseauth(uchar *a, int n, WAauth *w)
{
	Cbor c;
	char *why;
	int nid;

	memset(w, 0, sizeof *w);
	if(n < 37)
		return "bad authenticator data";
	memmove(w->rphash, a, WAhash);
	w->flags = a[32];
	w->count = (ulong)a[33]<<24 | a[34]<<16 | a[35]<<8 | a[36];
	if((w->flags & WAat) == 0){
		if(n != 37 && (w->flags & WAed) == 0)
			return "bad authenticator data";
		return nil;
	}
	if(n < 37+16+2)
		return "bad attested credential";
	memmove(w->aaguid, a+37, 16);
	nid = a[53]<<8 | a[54];
	if(nid < 16 || nid > WAmaxid || 55+nid > n)
		return "bad credential id";
	memmove(w->id, a+55, nid);
	w->nid = nid;
	c.p = a+55+nid;
	c.e = a+n;
	if((why = cosekey(&c, w->pub)) != nil)
		return why;
	if(!wapubok(w->pub))
		return "public key not on P-256";
	if(c.p != c.e && (w->flags & WAed) == 0)
		return "bad attested credential";
	return nil;
}

/* this relying party's, the user present, and verified if needuv */
char*
wacheckauth(WAauth *w, char *rpid, int needuv)
{
	uchar h[WAhash];

	sha2_256((uchar*)rpid, strlen(rpid), h, nil);
	if(tsmemcmp(w->rphash, h, sizeof h) != 0)
		return "not this site's passkey";
	if((w->flags & WAup) == 0)
		return "no user present";
	if(needuv && (w->flags & WAuv) == 0)
		return "the user not verified";
	if((w->flags & WAbs) != 0 && (w->flags & WAbe) == 0)
		return "bad authenticator data";
	return nil;
}

/*
 * the signature counter: an authenticator that keeps one gives one
 * above the last; 0 and 0 is one that keeps none (a synced passkey's)
 */
char*
wacounter(ulong last, ulong now)
{
	if(last == 0 && now == 0)
		return nil;
	if(now <= last)
		return "counter not above the last: a copied passkey?";
	return nil;
}

/* the assertion's signature (ES256, DER) over authenticatorData | sha256(clientDataJSON) */
char*
waverify(uchar pub[WApub], uchar *auth, int nauth, uchar *client, int nclient, uchar *sig, int nsig)
{
	uchar h[WAhash], d[WAhash];
	ECdomain dom;
	ECpub *p;
	DigestState *ds;
	char *e;

	if(nauth < 37 || nclient <= 0 || nsig <= 0)
		return "bad assertion";
	ecdominit(&dom, secp256r1);
	if((p = ecdecodepub(&dom, pub, WApub)) == nil){
		ecdomfree(&dom);
		return "bad public key";
	}
	sha2_256(client, nclient, h, nil);
	ds = sha2_256(auth, nauth, nil, nil);
	sha2_256(h, sizeof h, d, ds);
	e = X509ecdsaverifydigest(sig, nsig, d, sizeof d, &dom, p);
	ecpubfree(p);
	ecdomfree(&dom);
	return e == nil ? nil : "bad signature";
}

/* a point on P-256 */
int
wapubok(uchar pub[WApub])
{
	ECdomain dom;
	ECpub *p;

	ecdominit(&dom, secp256r1);
	p = ecdecodepub(&dom, pub, WApub);
	ecdomfree(&dom);
	if(p == nil)
		return 0;
	ecpubfree(p);
	return 1;
}

/* an SPKI's P-256 point (a browser's getPublicKey(): 26 bytes of header, then the point) */
int
waspkipub(char *spki, uchar pub[WApub])
{
	uchar key[256];
	int n;

	n = wab64udec(spki, key, sizeof key);
	if(n != 91 || key[26] != 4 || !wapubok(key+26))
		return -1;
	memmove(pub, key+26, WApub);
	return 0;
}
