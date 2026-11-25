#pragma	src	"/sys/src/libwebauthn"
#pragma	lib	"libwebauthn.a"

/*
 * WebAuthn (passkeys) on the relying party's side: what a server checks
 * of a browser's create and get.  Only ES256 (COSE -7, P-256) keys; no
 * attestation statement is verified (attestation "none").
 */

enum {
	WAup	= 0x01,	/* user present */
	WAuv	= 0x04,	/* user verified */
	WAbe	= 0x08,	/* backup eligible (a synced passkey's) */
	WAbs	= 0x10,	/* backed up */
	WAat	= 0x40,	/* attested credential data */
	WAed	= 0x80,	/* extensions */

	WAhash	= 32,	/* SHA-256 */
	WApub	= 65,	/* a P-256 point, uncompressed: 04 x y */
	WAmaxid	= 1023,	/* a credential id's bytes */
	WAchal	= 32,	/* a challenge's random bytes */
};

typedef struct WAauth WAauth;
struct WAauth
{
	uchar	rphash[WAhash];
	int	flags;
	ulong	count;
	/* the attested credential data (create): flags&WAat */
	uchar	aaguid[16];
	uchar	id[WAmaxid];
	int	nid;
	uchar	pub[WApub];
};

/* base64url without padding */
int	wab64uenc(char *s, int ns, uchar *p, int n);
int	wab64udec(char *s, uchar *p, int n);
int	wab64uok(char *s, int min, int max);

/*
 * Each check returns nil when it holds, else why not (a static string).
 */
char*	waclient(char *json, int n, char *type, char *chal, char **origins, int norigins);
char*	waparseauth(uchar *a, int n, WAauth *w);
char*	wacheckauth(WAauth *w, char *rpid, int needuv);
char*	wacounter(ulong last, ulong now);
char*	waverify(uchar pub[WApub], uchar *auth, int nauth, uchar *client, int nclient, uchar *sig, int nsig);
int	wapubok(uchar pub[WApub]);
int	waspkipub(char *spkib64u, uchar pub[WApub]);
void	wachallenge(char *s, int ns);
