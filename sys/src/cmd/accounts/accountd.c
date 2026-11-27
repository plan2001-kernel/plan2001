/*
 * accountd - Plan 9 accounts made and used with passkeys (WebAuthn),
 * without a password a person types (README.md, "Accounts and
 * passkeys").  It runs once, as the host owner of the auth and file
 * server, in a namespace with keyfs (/mnt/keys) and the file server's
 * console: what it makes is a genuine user - the file server's (its
 * /adm/users entry and home), the auth server's (a dp9ik key in keyfs,
 * random: nobody knows it as a password) - and the passkeys that name it.
 *
 * Connections, as aux/etherws makes them for a page (GET /account) and
 * auth/account and auth/asuser for the host owner: a pipe posted in /srv
 * and "conn SRVNAME KIND ADDR" written to /srv/SRVNAME (-s, accountd);
 * KIND account (a page's), or admin (only from a pipe the host owner
 * posted).  On the pipe a message is a write: T and its text; X ends it.
 *
 * A page's requests (B64 base64url without padding):
 *	challenge login			<- challenge C
 *	challenge enroll INVITE		<- challenge C NAME HANDLE
 *	challenge add TOKEN		<- challenge C NAME HANDLE [ID...]
 *	enroll INVITE ID AUTH CLIENT LABEL	<- ok TOKEN NAME EXPIRES
 *	login ID AUTH CLIENT SIG HANDLE	<- ok TOKEN NAME EXPIRES
 *	add TOKEN ID AUTH CLIENT LABEL	<- ok HASH
 *	session TOKEN			<- ok NAME EXPIRES
 *	creds TOKEN			<- ok, then a line a passkey: HASH LABEL CREATED LAST FLAGS
 *	revoke TOKEN HASH		<- ok
 *	logout TOKEN			<- ok
 * Another device for an account (device pairing), the account's device
 * with its session TOKEN, the new one with the KEY its claim gave:
 *	pair new TOKEN			<- ok CODE EXPIRES	(5 minutes; ends the account's earlier one)
 *	pair claim CODE			<- ok KEY CONFIRM NAME	(once)
 *	pair status TOKEN CODE		<- ok wait | claimed CONFIRM | approved | done | gone
 *	pair cancel TOKEN CODE		<- ok
 *	challenge pair TOKEN CODE	<- challenge C [ID...]	(once claimed)
 *	pair approve TOKEN CODE ID AUTH CLIENT SIG	<- ok	(one of the account's passkeys)
 *	pair wait KEY			<- ok claimed | approved | gone
 *	challenge pairadd KEY		<- challenge C NAME HANDLE [ID...]	(once approved)
 *	pair add KEY ID AUTH CLIENT LABEL	<- ok TOKEN NAME EXPIRES	(one passkey, then done)
 * CODE travels in the URL's fragment of a QR code; it gives nothing
 * without the approval on the account's device, where the person checks
 * that both show the same 6-digit CONFIRM.  CODE and KEY are 32 random
 * bytes, kept as their hashes in this process's memory.
 * and error WHY for any.  A challenge is 32 random bytes, this
 * connection's, for that one request (the next request uses it up, done
 * or not) within 2 minutes; a request on another connection may use it
 * too, found by its client data's challenge (a phone's page loses its
 * connection while another app makes the passkey); enroll's is bound to its invitation, add's
 * to its session, which must be recent (-R).  AUTH is authenticatorData,
 * CLIENT clientDataJSON, SIG the assertion's signature, HANDLE the
 * passkey's user handle (or -), LABEL a name for the passkey (B64 of
 * UTF-8, or -).  The passkey must be this rp's (-r), from one of its
 * origins (-o), with its user verified; only ES256.  enroll and add take
 * the key from the attested credential data (no attestation statement is
 * verified: attestation none); login checks the signature, the counter
 * (0 and 0 is a synced passkey's), and the account and its keyfs entry
 * enabled.  A session is 32 random bytes, kept only as their SHA-256 in
 * this process's memory (a restart ends them all), -t seconds; logout,
 * revoke (the passkey's) and disable end them.
 *
 * The host owner's (auth/account, auth/asuser):
 *	invite NAME TTL [GROUP...]	<- ok CODE	(shown once; kept as its hash)
 *	invites | uninvite HASH | users | creds NAME | revoke NAME HASH|all
 *	disable NAME | enable NAME | groups NAME | group NAME +GROUP|-GROUP
 *	sessions | logout NAME
 *	asuser TOKEN			<- ok NAME DOM KEY	(auth/asuser's: the
 *					session's user, the dp9ik key's hex)
 * An invitation names the account (a new name, NAME [a-z][a-z0-9]{1,26},
 * not reserved, not a user or group the file server or keyfs has) and
 * the groups it joins.  A group is one -g allows (a name, or a prefix and
 * *), never a user's own; one not there is made (newuser G :).  Nobody
 * chooses a uid, a name another has, or a group not allowed.
 *
 * What enroll makes, in order, under one lock: the account's record
 * (DIR/users/NAME, pending), its keyfs entry (a random AES key, expiry
 * never), the file server's user (newuser NAME on its console: done when
 * -a has it and -u/NAME is its), its groups, the passkey's record
 * (DIR/creds/HASH, made exclusively: a passkey is one account's), the
 * record active, then the invitation used up.  A step that fails undoes
 * the ones before it but the file server's user (a console can not take
 * one away): the record stays pending, and the next invitation for that
 * name takes the user over.  The log (syslog accountd, -l standard error)
 * has names and hashes, never a token, code or key.
 *
 *	auth/accountd -r rpid -o origin [-o origin]... -d authdom -c fscons
 *		[-D dir] [-k keys] [-u usr] [-a users] [-g group]... [-s srvname]
 *		[-t ttl] [-R recent] [-l]
 */
#include <u.h>
#include <libc.h>
#include <bio.h>
#include <mp.h>
#include <libsec.h>
#include <authsrv.h>
#include <thread.h>
#include <webauthn.h>
#include "conn.h"

enum {
	Stack	= 64*1024,
	Maxsess	= 16,	/* sessions of one user: a new one ends the oldest beyond */
	Chaltime	= 120,
	Ntoken	= 32,
	Nhandle	= 16,
	Ncode	= 24,
	Maxreq	= 64,	/* requests a connection */
	Maxcreds	= 64,
	Fswait	= 20*1000,	/* ms for the file server to show a console command done */
	Maxchals	= 1024,	/* challenges kept for another connection */
};

enum { Cnone, Clogin, Cenroll, Cadd, Cpair, Cpairadd };

/* a device pairing's states: made, claimed by the new device, approved, its passkey being made, done */
enum { Pwait, Pclaimed, Papproved, Padding, Pdone };
enum {
	Pairtime	= 5*60,	/* a pairing's life */
	Maxpairs	= 64,	/* pairings kept */
};

typedef struct Attr Attr;
typedef struct Conn Conn;
typedef struct Sess Sess;
typedef struct Chal Chal;
typedef struct Pair Pair;

struct Attr
{
	char	*k;
	char	*v;
	Attr	*next;
};

struct Conn
{
	int	fd;
	char	*addr;
	int	admin;
	/* its challenge */
	char	chal[64];
	int	op;
	char	*bound;	/* the invitation's hash or the session's user */
	char	handle[32];
	long	expires;
	int	adopted;	/* the challenge another connection was given */
};

/* a challenge given out, for a request on another connection */
struct Chal
{
	char	chal[64];
	int	op;
	char	*bound;
	char	handle[32];
	long	expires;
	Chal	*next;
};

/*
 * a device pairing: another device for an account, by a code the
 * account's device shows (a QR code's URL), claimed once by the new
 * device, which gets its own key, and approved on the account's device
 * with one of its passkeys; then the new device makes its own passkey.
 * Codes and keys are kept as their hashes only, in memory.
 */
struct Pair
{
	char	h[40];		/* the code's hash */
	char	kh[40];		/* the new device's key's hash, once claimed */
	char	*name;
	char	confirm[8];	/* the same number on both devices */
	int	state;
	long	expires;
	Pair	*next;
};

struct Sess
{
	uchar	h[SHA2_256dlen];
	char	*name;
	char	credh[40];
	long	created;
	long	expires;
	Sess	*next;
};

static char *rpid, *authdom, *fscons;
static char *origins[8];
static int norigins;
static char *dir = "/adm/accounts";
static char *keys = "/mnt/keys";
static char *usrdir = "/usr";
static char *admusers = "/adm/users";
static char *srvname = "accountd";
static char *allowed[32];
static int nallowed;
static long ttl = 7*24*60*60;
static long recent = 10*60;
static int logerr;
static char hostowner[64];

static QLock statelk;	/* the files: invitations, accounts, passkeys; the file server's users */
static QLock sesslk;
static Sess *sessions;
static QLock challk;
static Chal *chals;	/* newest first */
static QLock pairlk;	/* never held while taking statelk or sesslk */
static Pair *pairs;
static int nchals;

static char *reserved[] = {
	"glenda", "bootes", "adm", "sys", "none", "upas", "nobody", "root",
	"admin", "secstore", "webauthn", "cpu", "auth", "fs", "plan2001",
	"www", "web", "ether", "accounts", "accountd", "noworld", "daemon",
	"bin", "lib", "usr", "tmp", "mail", "uucp", "operator", "guest",
	nil,
};

static void
alog(char *fmt, ...)
{
	char buf[512];
	va_list arg;

	va_start(arg, fmt);
	vsnprint(buf, sizeof buf, fmt, arg);
	va_end(arg);
	if(logerr)
		fprint(2, "accountd: %s\n", buf);
	else
		syslog(0, "accountd", "%s", buf);
}

/* the files */

static void
freeattrs(Attr *a)
{
	Attr *n;

	for(; a != nil; a = n){
		n = a->next;
		free(a->k);
		free(a->v);
		free(a);
	}
}

/* a file of lines KEY VALUE; nil if there is none */
static Attr*
readattrs(char *path)
{
	Biobuf *b;
	Attr *a, **l;
	char *s, *v;

	if((b = Bopen(path, OREAD)) == nil)
		return nil;
	a = nil;
	l = &a;
	while((s = Brdstr(b, '\n', 1)) != nil){
		v = s + strcspn(s, " \t");
		if(*v != 0)
			*v++ = 0;
		v += strspn(v, " \t");
		if(*s != 0){
			*l = mallocz(sizeof **l, 1);
			(*l)->k = strdup(s);
			(*l)->v = strdup(v);
			l = &(*l)->next;
		}
		free(s);
	}
	Bterm(b);
	if(a == nil)
		a = mallocz(sizeof *a, 1);	/* an empty file is there */
	return a;
}

static char*
aval(Attr *a, char *k)
{
	for(; a != nil; a = a->next)
		if(a->k != nil && strcmp(a->k, k) == 0)
			return a->v;
	return nil;
}

/*
 * the whole file, made (excl: only if it is not there) or rewritten.  A
 * rewrite is one write over the old record, never truncated first (a
 * crash or a full disk must not leave a credential without its key): a
 * shorter record is padded with newlines, which readattrs skips.
 */
static int
writefile(char *path, int excl, char *s)
{
	int fd, n;
	vlong old;
	Dir *d;
	char *w;

	if(excl)
		fd = create(path, OWRITE|OEXCL, 0600);
	else if((fd = open(path, OWRITE)) < 0)
		fd = create(path, OWRITE, 0600);
	if(fd < 0)
		return -1;
	old = 0;
	if(!excl && (d = dirfstat(fd)) != nil){
		old = d->length;
		free(d);
	}
	n = strlen(s);
	w = s;
	if(old > n && old < 64*1024){
		if((w = malloc(old)) == nil){
			close(fd);
			return -1;
		}
		memmove(w, s, n);
		memset(w+n, '\n', old-n);
		n = old;
	}
	if(pwrite(fd, w, n, 0) != n){
		if(w != s)
			free(w);
		close(fd);
		return -1;
	}
	if(w != s)
		free(w);
	return close(fd);
}

static char*
pathf(char *fmt, ...)
{
	va_list arg;
	char *p;

	va_start(arg, fmt);
	p = vsmprint(fmt, arg);
	va_end(arg);
	return p;
}

static int
exists(char *path)
{
	Dir *d;

	d = dirstat(path);
	free(d);
	return d != nil;
}

/* the first 16 bytes of SHA-256, hex: a name for a file */
static void
hashname(char *out, uchar *p, int n)
{
	uchar h[SHA2_256dlen];

	sha2_256(p, n, h, nil);
	enc16(out, 33, h, 16);
	out[32] = 0;
	for(; *out; out++)
		if(*out >= 'A' && *out <= 'Z')
			*out += 'a' - 'A';
}

static int
ishash(char *s)
{
	return strlen(s) == 32 && strspn(s, "0123456789abcdef") == 32;
}

static char*
label(char *s)
{
	uchar b[128];
	int i, n;

	if(strcmp(s, "-") == 0)
		return "-";
	if(!wab64uok(s, 1, 172) || (n = wab64udec(s, b, sizeof b)) <= 0 || n > 64)
		return nil;
	for(i = 0; i < n; i++)
		if(b[i] < 0x20 || b[i] == 0x7f)
			return nil;
	return s;
}

/* names */

static int
nameform(char *s)
{
	int n;

	n = strlen(s);
	return n >= 2 && n <= 27 && s[0] >= 'a' && s[0] <= 'z'
		&& strspn(s, "abcdefghijklmnopqrstuvwxyz0123456789") == n;
}

static int
isreserved(char *s)
{
	char **r;

	if(strcmp(s, hostowner) == 0)
		return 1;
	for(r = reserved; *r != nil; r++)
		if(strcmp(*r, s) == 0)
			return 1;
	return 0;
}

/*
 * the file server's users file (id:name:leader:members): name's id (-1
 * not there), whether member is among its members; -2 if it can not be read
 */
static int
fsuser(char *name, char *member, int *in)
{
	Biobuf *b;
	char *l, *f[4], *m[256];
	int i, n, id;

	if(in != nil)
		*in = 0;
	if((b = Bopen(admusers, OREAD)) == nil)
		return -2;
	id = -1;
	while(id < 0 && (l = Brdstr(b, '\n', 1)) != nil){
		if(getfields(l, f, nelem(f), 0, ":") == 4 && strcmp(f[1], name) == 0){
			id = atoi(f[0]);
			if(member != nil && in != nil){
				n = getfields(f[3], m, nelem(m), 0, ",");
				for(i = 0; i < n; i++)
					if(strcmp(m[i], member) == 0)
						*in = 1;
			}
		}
		free(l);
	}
	Bterm(b);
	return id;
}

/* a group the host owner may put an account in */
static char*
groupok(char *g)
{
	int i, n, id;

	if(!nameform(g) || isreserved(g))
		return "not a group one may join";
	for(i = 0; i < nallowed; i++){
		n = strlen(allowed[i]);
		if(n > 0 && allowed[i][n-1] == '*' ? strncmp(g, allowed[i], n-1) == 0 : strcmp(g, allowed[i]) == 0)
			break;
	}
	if(i == nallowed)
		return "not a group one may join";
	id = fsuser(g, nil, nil);
	if(id == -2)
		return "no users file";
	/* a user's own group is the user: groups have ids from 10000 (newuser G :) */
	if(id >= 0 && id < 10000)
		return "a user, not a group";
	if(exists(pathf("%s/%s", keys, g)))
		return "a user, not a group";
	return nil;
}

/* a name for a new account (pending: one whose making did not end takes it over) */
static char*
newnameok(char *name, int *pending)
{
	Attr *a;
	char *p, *st;

	*pending = 0;
	if(!nameform(name))
		return "bad name";
	if(isreserved(name))
		return "reserved name";
	p = pathf("%s/users/%s", dir, name);
	a = readattrs(p);
	free(p);
	if(a != nil){
		st = aval(a, "state");
		*pending = st != nil && strcmp(st, "pending") == 0;
		freeattrs(a);
		return *pending ? nil : "name taken";
	}
	switch(fsuser(name, nil, nil)){
	case -2:
		return "no users file";
	case -1:
		break;
	default:
		return "name taken";
	}
	p = pathf("%s/%s", keys, name);
	if(exists(p)){
		free(p);
		return "name taken";
	}
	free(p);
	return nil;
}

/* the file server's console: a command, then wait until done(arg) */
static char*
fscmd(char *cmd, int (*done)(char*, char*), char *a0, char *a1)
{
	int fd, n, i;
	char *s;

	if(fscons == nil)
		return "no file server console";
	if((fd = open(fscons, OWRITE)) < 0)
		return "can not open the file server's console";
	s = smprint("%s\n", cmd);
	n = strlen(s);
	i = write(fd, s, n);
	free(s);
	close(fd);
	if(i != n)
		return "file server console";
	for(i = 0; i < Fswait/200; i++){
		if((*done)(a0, a1))
			return nil;
		sleep(200);
	}
	return "the file server did not do it";
}

/* name a user with its home */
static int
userdone(char *name, char*)
{
	Dir *d;
	char *p;
	int ok;

	if(fsuser(name, nil, nil) < 0)
		return 0;
	p = pathf("%s/%s", usrdir, name);
	d = dirstat(p);
	free(p);
	ok = d != nil && (d->mode & DMDIR) != 0 && strcmp(d->uid, name) == 0;
	free(d);
	return ok;
}

static int
groupthere(char *g, char*)
{
	return fsuser(g, nil, nil) >= 10000;
}

static int
memberin(char *g, char *name)
{
	int in;

	return fsuser(g, name, &in) >= 0 && in;
}

static int
memberout(char *g, char *name)
{
	int in;

	return fsuser(g, name, &in) >= 0 && !in;
}

/* name in group g (or out: -) */
static char*
setgroup(char *name, char *g, int add)
{
	char *e, *c;
	int in;

	if((e = groupok(g)) != nil)
		return e;
	if(fsuser(name, nil, nil) < 0)
		return "no such user";
	if(add && fsuser(g, nil, nil) < 0){
		c = smprint("newuser %s :", g);
		e = fscmd(c, groupthere, g, nil);
		free(c);
		if(e != nil)
			return e;
	}
	fsuser(g, name, &in);
	if(in == add)
		return nil;
	c = smprint("newuser %s %c%s", g, add ? '+' : '-', name);
	e = fscmd(c, add ? memberin : memberout, g, name);
	free(c);
	return e;
}

/* keyfs */

static char*
keyfsread(char *name, char *file, char *buf, int n)
{
	char *p;
	int fd, m;

	p = pathf("%s/%s/%s", keys, name, file);
	fd = open(p, OREAD);
	free(p);
	if(fd < 0)
		return nil;
	m = read(fd, buf, n-1);
	close(fd);
	if(m < 0)
		return nil;
	buf[m] = 0;
	buf[strcspn(buf, "\n")] = 0;
	return buf;
}

static int
keyfswrite(char *name, char *file, void *a, int n)
{
	char *p;
	int fd, m;

	p = pathf("%s/%s/%s", keys, name, file);
	fd = open(p, OWRITE);
	free(p);
	if(fd < 0)
		return -1;
	m = write(fd, a, n);
	close(fd);
	return m == n ? 0 : -1;
}

/* keyfs's entry: a random dp9ik key */
static char*
mkkey(char *name)
{
	uchar aes[AESKEYLEN];
	char *p;
	int fd;

	p = pathf("%s/%s", keys, name);
	if(!exists(p)){
		if((fd = create(p, OREAD, DMDIR|0700)) < 0){
			free(p);
			return "keyfs: can not make the user";
		}
		close(fd);
	}
	free(p);
	genrandom(aes, sizeof aes);
	fd = keyfswrite(name, "aeskey", aes, sizeof aes);
	memset(aes, 0, sizeof aes);
	if(fd < 0)
		return "keyfs: can not keep the key (keyfs not in aes format?)";
	if(keyfswrite(name, "expire", "never", 5) < 0 || keyfswrite(name, "status", "ok", 2) < 0)
		return "keyfs: can not enable the user";
	return nil;
}

static void
rmkey(char *name)
{
	char *p;

	p = pathf("%s/%s", keys, name);
	remove(p);
	free(p);
}

static int
keyenabled(char *name)
{
	char buf[32];

	return keyfsread(name, "status", buf, sizeof buf) != nil && strcmp(buf, "ok") == 0;
}

/* sessions */

static void
endsessions(char *name, char *credh)
{
	Sess *s, **l;

	qlock(&sesslk);
	for(l = &sessions; (s = *l) != nil; ){
		if(name == nil || strcmp(s->name, name) == 0 && (credh == nil || strcmp(s->credh, credh) == 0)){
			*l = s->next;
			free(s->name);
			free(s);
			continue;
		}
		l = &s->next;
	}
	qunlock(&sesslk);
}

/* a user's sessions beyond Maxsess, the oldest first, and the ended: gone (sesslk held) */
static void
trimsessions(char *name)
{
	Sess *s, **l;
	long now;
	int n;

	now = time(0);
	n = 0;
	for(l = &sessions; (s = *l) != nil; ){
		/* the list is newest first */
		if(s->expires <= now || strcmp(s->name, name) == 0 && ++n > Maxsess){
			*l = s->next;
			free(s->name);
			free(s);
			continue;
		}
		l = &s->next;
	}
}

static void
newsession(char *name, char *credh, char *tok, int ntok, long *expires)
{
	uchar t[Ntoken];
	Sess *s;

	genrandom(t, sizeof t);
	wab64uenc(tok, ntok, t, sizeof t);
	s = mallocz(sizeof *s, 1);
	sha2_256((uchar*)tok, strlen(tok), s->h, nil);
	memset(t, 0, sizeof t);
	s->name = strdup(name);
	strecpy(s->credh, s->credh+sizeof s->credh, credh);
	s->created = time(0);
	s->expires = s->created + ttl;
	*expires = s->expires;
	qlock(&sesslk);
	s->next = sessions;
	sessions = s;
	trimsessions(name);
	qunlock(&sesslk);
}

/* the session tok names (its user's name, malloced), those ended gone */
static char*
findsession(char *tok, long *created, long *expires)
{
	uchar h[SHA2_256dlen];
	Sess *s, **l;
	char *name;
	long now;

	if(!wab64uok(tok, 40, 48))
		return nil;
	sha2_256((uchar*)tok, strlen(tok), h, nil);
	now = time(0);
	name = nil;
	qlock(&sesslk);
	for(l = &sessions; (s = *l) != nil; ){
		if(s->expires <= now){
			*l = s->next;
			free(s->name);
			free(s);
			continue;
		}
		if(name == nil && tsmemcmp(s->h, h, sizeof h) == 0){
			name = strdup(s->name);
			if(created != nil)
				*created = s->created;
			if(expires != nil)
				*expires = s->expires;
		}
		l = &s->next;
	}
	qunlock(&sesslk);
	return name;
}

static int
logoutsession(char *tok)
{
	uchar h[SHA2_256dlen];
	Sess *s, **l;
	int n;

	sha2_256((uchar*)tok, strlen(tok), h, nil);
	n = 0;
	qlock(&sesslk);
	for(l = &sessions; (s = *l) != nil; ){
		if(tsmemcmp(s->h, h, sizeof h) == 0){
			*l = s->next;
			free(s->name);
			free(s);
			n++;
			continue;
		}
		l = &s->next;
	}
	qunlock(&sesslk);
	return n;
}

/* device pairings (pairlk held) */

static void
freepair(Pair *p)
{
	free(p->name);
	memset(p, 0, sizeof *p);
	free(p);
}

/* the ended gone; the number left */
static int
trimpairs(void)
{
	Pair *p, **l;
	long now;
	int n;

	now = time(0);
	n = 0;
	for(l = &pairs; (p = *l) != nil; ){
		if(p->expires <= now){
			*l = p->next;
			freepair(p);
			continue;
		}
		n++;
		l = &p->next;
	}
	return n;
}

/* by its code (B64, as a session token) */
static Pair*
findpair(char *code)
{
	char h[40];
	Pair *p;

	if(!wab64uok(code, 40, 48))
		return nil;
	hashname(h, (uchar*)code, strlen(code));
	trimpairs();
	for(p = pairs; p != nil; p = p->next)
		if(tsmemcmp((uchar*)p->h, (uchar*)h, 32) == 0)
			return p;
	return nil;
}

/* by the new device's key, while it is the new device's to use (not done) */
static Pair*
findpairkey(char *key)
{
	char h[40];
	Pair *p;

	if(!wab64uok(key, 40, 48))
		return nil;
	hashname(h, (uchar*)key, strlen(key));
	trimpairs();
	for(p = pairs; p != nil; p = p->next)
		if(p->state != Pwait && p->state != Pdone && tsmemcmp((uchar*)p->kh, (uchar*)h, 32) == 0)
			return p;
	return nil;
}

static void
droppair(Pair *d)
{
	Pair *p, **l;

	for(l = &pairs; (p = *l) != nil; l = &p->next)
		if(p == d){
			*l = p->next;
			freepair(p);
			return;
		}
}

/* a random token (B64) in tok, its hash in h */
static void
newtoken(char *tok, int ntok, char *h)
{
	uchar t[Ntoken];

	genrandom(t, sizeof t);
	wab64uenc(tok, ntok, t, sizeof t);
	memset(t, 0, sizeof t);
	hashname(h, (uchar*)tok, strlen(tok));
}

/* invitations, accounts and passkeys (statelk held) */

/* the invitation CODE's record and hash; nil (why) if there is none, or it has ended */
static Attr*
invitation(char *code, char *h, char **why)
{
	Attr *a;
	char *p, *e;

	*why = "no such invitation";
	if(!wab64uok(code, 32, 32))
		return nil;
	hashname(h, (uchar*)code, strlen(code));
	p = pathf("%s/invites/%s", dir, h);
	a = readattrs(p);
	if(a == nil){
		free(p);
		return nil;
	}
	if((e = aval(a, "expires")) == nil || strtol(e, nil, 10) <= time(0) || aval(a, "name") == nil){
		remove(p);	/* ended: gone */
		free(p);
		freeattrs(a);
		*why = "the invitation has ended";
		return nil;
	}
	free(p);
	return a;
}

static Attr*
account(char *name)
{
	Attr *a;
	char *p;

	if(!nameform(name))
		return nil;
	p = pathf("%s/users/%s", dir, name);
	a = readattrs(p);
	free(p);
	return a;
}

/* the account is there and enabled (not pending, not disabled) */
static int
accountactive(char *name)
{
	Attr *a;
	char *v;
	int ok;

	a = account(name);
	ok = a != nil && (v = aval(a, "state")) != nil && strcmp(v, "active") == 0;
	freeattrs(a);
	return ok;
}

static Attr*
cred(char *h)
{
	Attr *a;
	char *p;

	if(!ishash(h))
		return nil;
	p = pathf("%s/creds/%s", dir, h);
	a = readattrs(p);
	free(p);
	return a;
}

static char*
credtext(char *name, char *id, uchar pub[WApub], ulong count, char *lab, int flags, long created, long last)
{
	char pb[128];

	wab64uenc(pb, sizeof pb, pub, WApub);
	return smprint("name %s\nid %s\npubkey %s\ncount %lud\nlabel %s\nflags %d\ncreated %ld\nlast %ld\n",
		name, id, pb, count, lab, flags, created, last);
}

static int
countcreds(char *name)
{
	Dir *d;
	Attr *a;
	char *p, *v;
	int fd, n, i, c;

	p = pathf("%s/creds", dir);
	fd = open(p, OREAD);
	free(p);
	if(fd < 0)
		return 0;
	c = 0;
	while((n = dirread(fd, &d)) > 0){
		for(i = 0; i < n; i++){
			if((a = cred(d[i].name)) == nil)
				continue;
			if((v = aval(a, "name")) != nil && strcmp(v, name) == 0)
				c++;
			freeattrs(a);
		}
		free(d);
	}
	close(fd);
	return c;
}

/* each passkey of name's: fn(its hash, its record, arg) */
static void
eachcred(char *name, void (*fn)(char*, Attr*, void*), void *arg)
{
	Dir *d;
	Attr *a;
	char *p, *v;
	int fd, n, i;

	p = pathf("%s/creds", dir);
	fd = open(p, OREAD);
	free(p);
	if(fd < 0)
		return;
	while((n = dirread(fd, &d)) > 0){
		for(i = 0; i < n; i++){
			if((a = cred(d[i].name)) == nil)
				continue;
			if((v = aval(a, "name")) != nil && (name == nil || strcmp(v, name) == 0))
				fn(d[i].name, a, arg);
			freeattrs(a);
		}
		free(d);
	}
	close(fd);
}

/* the checks */

typedef struct Req Req;
struct Req
{
	uchar	auth[1024];
	int	nauth;
	uchar	client[4096];
	int	nclient;
	uchar	sig[256];
	int	nsig;
	uchar	id[WAmaxid];
	int	nid;
	char	idh[40];
	WAauth	w;
};

static void
freechal(Chal *h)
{
	free(h->bound);
	free(h);
}

/* the connection's new challenge, kept for another connection too (the oldest go beyond Maxchals) */
static void
keepchal(Conn *c)
{
	Chal *h, **l;
	int n;

	h = mallocz(sizeof *h, 1);
	strecpy(h->chal, h->chal+sizeof h->chal, c->chal);
	h->op = c->op;
	h->bound = c->bound ? strdup(c->bound) : nil;
	strecpy(h->handle, h->handle+sizeof h->handle, c->handle);
	h->expires = c->expires;
	qlock(&challk);
	h->next = chals;
	chals = h;
	n = 0;
	for(l = &chals; *l != nil;){
		if((*l)->expires <= time(0) || n >= Maxchals){
			h = *l;
			*l = h->next;
			freechal(h);
			continue;
		}
		n++;
		l = &(*l)->next;
	}
	nchals = n;
	qunlock(&challk);
}

/*
 * the kept challenge s gone, used here: 1 if another connection used it
 * already (one gone beyond Maxchals is this connection's still)
 */
static int
dropchal(char *s)
{
	Chal *h, **l;
	int used;

	used = 0;
	qlock(&challk);
	for(l = &chals; *l != nil; l = &(*l)->next)
		if(strcmp((*l)->chal, s) == 0){
			h = *l;
			used = h->op == Cnone;
			*l = h->next;
			freechal(h);
			nchals--;
			break;
		}
	qunlock(&challk);
	return used;
}

/*
 * a connection without a challenge (a new one): the kept challenge its
 * client data (B64 client64, of type) has, as this connection's
 */
static void
adoptchal(Conn *c, char *client64, char *type)
{
	uchar client[4096];
	int n;
	Chal *h, **l;

	if(c->op != Cnone || !wab64uok(client64, 20, 5400) || (n = wab64udec(client64, client, sizeof client)) <= 0)
		return;
	qlock(&challk);
	for(l = &chals; *l != nil; l = &(*l)->next){
		h = *l;
		if(h->op == Cnone || h->expires <= time(0) || waclient((char*)client, n, type, h->chal, origins, norigins) != nil)
			continue;
		/* kept, used (Cnone) until it ends: its own connection can not use it again */
		strecpy(c->chal, c->chal+sizeof c->chal, h->chal);
		c->op = h->op;
		free(c->bound);
		c->bound = h->bound ? strdup(h->bound) : nil;
		strecpy(c->handle, c->handle+sizeof c->handle, h->handle);
		c->expires = h->expires;
		c->adopted = 1;
		h->op = Cnone;
		break;
	}
	qunlock(&challk);
}

/* the challenge this connection had for op (used up) */
static char*
takechal(Conn *c, int op, char *bound)
{
	int had, ok;

	had = c->op;
	if(had != Cnone && !c->adopted && dropchal(c->chal)){
		c->op = Cnone;
		return "the challenge is used";
	}
	c->adopted = 0;
	ok = had == op && c->expires > time(0)
		&& (bound == nil || c->bound != nil && strcmp(c->bound, bound) == 0);
	c->op = Cnone;
	if(ok)
		return nil;
	if(had == op && c->expires <= time(0))
		return "the challenge has ended";
	return "no challenge for this";
}

static char*
ceremony(Conn *c, Req *r, char *id, char *auth, char *client, char *sig, char *type, int create)
{
	char *e;

	if(!wab64uok(id, 22, 1400) || (r->nid = wab64udec(id, r->id, sizeof r->id)) < 16)
		return "bad passkey id";
	hashname(r->idh, r->id, r->nid);
	if(!wab64uok(auth, 50, 1400) || (r->nauth = wab64udec(auth, r->auth, sizeof r->auth)) < 37)
		return "bad authenticator data";
	if(!wab64uok(client, 20, 5400) || (r->nclient = wab64udec(client, r->client, sizeof r->client)) <= 0)
		return "bad client data";
	if(sig != nil && (!wab64uok(sig, 8, 340) || (r->nsig = wab64udec(sig, r->sig, sizeof r->sig)) <= 0))
		return "bad signature";
	if((e = waclient((char*)r->client, r->nclient, type, c->chal, origins, norigins)) != nil)
		return e;
	if((e = waparseauth(r->auth, r->nauth, &r->w)) != nil)
		return e;
	if((e = wacheckauth(&r->w, rpid, 1)) != nil)
		return e;
	if(create){
		if((r->w.flags & WAat) == 0)
			return "no attested credential";
		if(r->w.nid != r->nid || memcmp(r->w.id, r->id, r->nid) != 0)
			return "not the attested passkey";
	}else if(r->w.flags & WAat)
		return "bad assertion";
	return nil;
}

/* enroll INVITE ID AUTH CLIENT LABEL: a new account and its first passkey */
static char*
enroll(Conn *c, char **f, char *tok, int ntok, char **namep, long *expires)
{
	char ih[40], *why, *name, *lab, *p, *s, *gs, *g[32];
	int ng, i, pending, keymade, nundo, made;
	Attr *inv, *acct;
	Req *r;

	adoptchal(c, f[4], "webauthn.create");
	if((why = takechal(c, Cenroll, nil)) != nil)
		return why;
	if((lab = label(f[5])) == nil)
		return "bad label";
	r = mallocz(sizeof *r, 1);
	if((why = ceremony(c, r, f[2], f[3], f[4], nil, "webauthn.create", 1)) != nil){
		free(r);
		return why;
	}

	qlock(&statelk);
	name = nil;
	acct = nil;
	gs = nil;
	nundo = 0;
	if((inv = invitation(f[1], ih, &why)) == nil)
		goto out;
	if(c->bound == nil || strcmp(c->bound, ih) != 0){
		why = "not this invitation's challenge";
		goto out;
	}
	name = strdup(aval(inv, "name"));
	if((why = newnameok(name, &pending)) != nil)
		goto out;
	p = pathf("%s/creds/%s", dir, r->idh);
	if(exists(p)){
		free(p);
		why = "the passkey is known";
		goto out;
	}
	free(p);
	gs = strdup(aval(inv, "groups") ? aval(inv, "groups") : "");
	ng = tokenize(gs, g, nelem(g));
	for(i = 0; i < ng; i++)
		if((why = groupok(g[i])) != nil)
			goto out;

	/* the record: pending until all of it is there */
	acct = account(name);
	p = pathf("%s/users/%s", dir, name);
	s = smprint("state pending\nhandle %s\ncreated %ld\n",
		acct && aval(acct, "handle") ? aval(acct, "handle") : c->handle, time(0));
	i = writefile(p, 0, s);
	free(s);
	free(p);
	if(i < 0){
		why = "can not keep the account";
		goto out;
	}
	alog("enroll %s: begun%s", name, pending ? " (taking over a pending one)" : "");

	keymade = !exists(s = pathf("%s/%s", keys, name));
	free(s);
	if((why = mkkey(name)) != nil)
		goto undo;
	if(fsuser(name, nil, nil) < 0){
		s = smprint("newuser %s", name);
		why = fscmd(s, userdone, name, nil);
		free(s);
		if(why != nil)
			goto undo;
	}else if(!userdone(name, nil)){
		why = "the file server's user has no home of its own";
		goto undo;
	}
	for(; nundo < ng; nundo++){
		made = !memberin(g[nundo], name);
		if((why = setgroup(name, g[nundo], 1)) != nil)
			goto undo;
		if(!made)
			g[nundo] = nil;	/* it was in it: it stays */
	}
	p = pathf("%s/creds/%s", dir, r->idh);
	s = credtext(name, f[2], r->w.pub, r->w.count, lab, r->w.flags, time(0), 0);
	i = writefile(p, 1, s);
	free(s);
	free(p);
	if(i < 0){
		why = "the passkey is known";
		goto undo;
	}
	p = pathf("%s/users/%s", dir, name);
	s = smprint("state active\nhandle %s\ncreated %ld\n",
		acct && aval(acct, "handle") ? aval(acct, "handle") : c->handle, time(0));
	i = writefile(p, 0, s);
	free(s);
	free(p);
	if(i < 0){
		p = pathf("%s/creds/%s", dir, r->idh);
		remove(p);
		free(p);
		why = "can not keep the account";
		goto undo;
	}
	p = pathf("%s/invites/%s", dir, ih);
	if(remove(p) < 0)
		alog("enroll %s: the invitation %s not removed: %r", name, ih);
	free(p);
	alog("enroll %s: done, passkey %s", name, r->idh);
	newsession(name, r->idh, tok, ntok, expires);
	*namep = name;
	name = nil;
	why = nil;
	goto out;

undo:
	alog("enroll %s: undone: %s", name, why);
	while(nundo > 0)
		if(g[--nundo] != nil)
			setgroup(name, g[nundo], 0);
	if(keymade)
		rmkey(name);
	/* the record stays pending: the file server's user, if made, is its */
out:
	qunlock(&statelk);
	freeattrs(inv);
	freeattrs(acct);
	free(gs);
	free(name);
	free(r);
	return why;
}

/*
 * an assertion (r, from ceremony) by a passkey of an enabled account:
 * the passkey's id (B64 id), its user handle (or -), the signature and
 * the counter checked, the counter and last use kept; its account's
 * name (malloced) in *namep (statelk held)
 */
static char*
assertion(Req *r, char *id, char *handle, char **namep)
{
	uchar pub[WApub];
	char *why, *name, *p, *s, *v;
	Attr *cr, *acct;
	ulong last;

	*namep = nil;
	acct = nil;
	name = nil;
	if((cr = cred(r->idh)) == nil || (v = aval(cr, "id")) == nil || strcmp(v, id) != 0){
		why = "no such passkey";
		goto out;
	}
	name = strdup(aval(cr, "name"));
	if((acct = account(name)) == nil || (v = aval(acct, "state")) == nil || strcmp(v, "active") != 0){
		why = "the account is not enabled";
		goto out;
	}
	if(strcmp(handle, "-") != 0 && ((v = aval(acct, "handle")) == nil || strcmp(v, handle) != 0)){
		why = "not this account's passkey";
		goto out;
	}
	if(!keyenabled(name)){
		why = "the account is not enabled";
		goto out;
	}
	if((v = aval(cr, "pubkey")) == nil || wab64udec(v, pub, sizeof pub) != WApub){
		why = "no such passkey";
		goto out;
	}
	last = strtoul(aval(cr, "count") ? aval(cr, "count") : "0", nil, 10);
	if((why = wacounter(last, r->w.count)) != nil)
		goto out;
	if((why = waverify(pub, r->auth, r->nauth, r->client, r->nclient, r->sig, r->nsig)) != nil)
		goto out;
	p = pathf("%s/creds/%s", dir, r->idh);
	s = credtext(name, id, pub, r->w.count, aval(cr, "label") ? aval(cr, "label") : "-",
		atoi(aval(cr, "flags") ? aval(cr, "flags") : "0"),
		strtol(aval(cr, "created") ? aval(cr, "created") : "0", nil, 10), time(0));
	if(writefile(p, 0, s) < 0)
		why = "can not keep the counter";
	free(s);
	free(p);
	if(why == nil){
		*namep = name;
		name = nil;
	}
out:
	freeattrs(cr);
	freeattrs(acct);
	free(name);
	return why;
}

/* login ID AUTH CLIENT SIG HANDLE: a session for a passkey's account */
static char*
login(Conn *c, char **f, char *tok, int ntok, char **namep, long *expires)
{
	char *why, *name;
	Req *r;

	adoptchal(c, f[3], "webauthn.get");
	if((why = takechal(c, Clogin, nil)) != nil)
		return why;
	r = mallocz(sizeof *r, 1);
	if((why = ceremony(c, r, f[1], f[2], f[3], f[4], "webauthn.get", 0)) != nil){
		free(r);
		return why;
	}
	qlock(&statelk);
	why = assertion(r, f[1], f[5], &name);
	qunlock(&statelk);
	if(why == nil){
		newsession(name, r->idh, tok, ntok, expires);
		*namep = name;
	}
	free(r);
	return why;
}

/* add TOKEN ID AUTH CLIENT LABEL: another passkey for the session's account */
static char*
addcred(Conn *c, char **f, char *idh)
{
	char *why, *name, *lab, *p, *s;
	long created;
	Req *r;
	int i;

	adoptchal(c, f[4], "webauthn.create");
	if((name = findsession(f[1], &created, nil)) == nil){
		takechal(c, Cadd, nil);
		return "no such session";
	}
	lab = label(f[5]);
	why = takechal(c, Cadd, name);
	if(why == nil && created + recent < time(0))
		why = "sign in again first";
	if(why == nil && lab == nil)
		why = "bad label";
	if(why != nil){
		free(name);
		return why;
	}
	r = mallocz(sizeof *r, 1);
	if((why = ceremony(c, r, f[2], f[3], f[4], nil, "webauthn.create", 1)) != nil)
		goto out;
	qlock(&statelk);
	if(countcreds(name) >= Maxcreds)
		why = "too many passkeys";
	else{
		p = pathf("%s/creds/%s", dir, r->idh);
		s = credtext(name, f[2], r->w.pub, r->w.count, lab, r->w.flags, time(0), 0);
		i = writefile(p, 1, s);
		free(s);
		free(p);
		if(i < 0)
			why = "the passkey is known";
	}
	qunlock(&statelk);
	if(why == nil){
		strcpy(idh, r->idh);
		alog("add %s: passkey %s", name, r->idh);
	}
out:
	free(r);
	free(name);
	return why;
}

typedef struct List List;
struct List
{
	char	*s;
	int	n;
};

static void
listone(char *h, Attr *a, void *arg)
{
	List *l;
	char *s;

	l = arg;
	if(l->n >= Maxcreds)
		return;
	s = smprint("%s\n%s %s %s %s %s %s", l->s, h, aval(a, "name"),
		aval(a, "label") ? aval(a, "label") : "-",
		aval(a, "created") ? aval(a, "created") : "0",
		aval(a, "last") ? aval(a, "last") : "0",
		aval(a, "flags") ? aval(a, "flags") : "0");
	free(l->s);
	l->s = s;
	l->n++;
}

static void
idone(char *, Attr *a, void *arg)
{
	List *l;
	char *s;

	l = arg;
	if(l->n >= Maxcreds || aval(a, "id") == nil || strlen(l->s) + strlen(aval(a, "id")) > Maxmsg/2)
		return;
	s = smprint("%s %s", l->s, aval(a, "id"));
	free(l->s);
	l->s = s;
	l->n++;
}

static char*
listcreds(char *name)
{
	List l;

	l.s = strdup("ok");
	l.n = 0;
	eachcred(name, listone, &l);
	return l.s;
}

static void
rmone(char *h, Attr*, void*)
{
	char *p;

	p = pathf("%s/creds/%s", dir, h);
	remove(p);
	free(p);
}

/* name's passkey h (or all), its sessions ended; it may not be its last unless all */
static char*
revokecred(char *name, char *h, int lastok)
{
	Attr *a;
	char *p, *v;

	if(strcmp(h, "all") == 0){
		if(!lastok)
			return "not allowed";
		eachcred(name, rmone, nil);
		endsessions(name, nil);
		alog("revoke %s: all passkeys", name);
		return nil;
	}
	if((a = cred(h)) == nil || (v = aval(a, "name")) == nil || strcmp(v, name) != 0){
		freeattrs(a);
		return "no such passkey";
	}
	freeattrs(a);
	if(!lastok && countcreds(name) <= 1)
		return "it is the account's last passkey";
	p = pathf("%s/creds/%s", dir, h);
	if(remove(p) < 0){
		free(p);
		return "can not remove it";
	}
	free(p);
	endsessions(name, h);
	alog("revoke %s: passkey %s", name, h);
	return nil;
}

/* a page's requests */

static void
reply(Conn *c, char *fmt, ...)
{
	char *s, *m;
	va_list arg;
	int n;

	va_start(arg, fmt);
	s = vsmprint(fmt, arg);
	va_end(arg);
	m = smprint("T%s", s);
	free(s);
	n = strlen(m);
	if(n > Maxmsg)
		n = Maxmsg;
	write(c->fd, m, n);
	free(m);
}

/*
 * a page's device pairing requests (pagereq's): nil when answered, else
 * why not.  The account's device (its session TOKEN) makes the pairing
 * and shows its CODE; the new device claims it once and gets its own KEY
 * and a CONFIRM number, the same the account's device shows; the
 * account's device approves with one of the account's passkeys; then the
 * new device makes its own passkey for the account and gets a session.
 */
static char*
pairreq(Conn *c, char **f, int nf)
{
	char code[64], key[64], h[40], tok[64], conf[8], *name, *who, *why, *s, *p0, *lab;
	uchar b[4];
	long expires;
	Attr *acct;
	List ids;
	Pair *p;
	Req *r;
	int i;

	name = nil;
	who = nil;
	why = nil;
	h[0] = 0;
	if(strcmp(f[0], "challenge") == 0 && nf == 4 && strcmp(f[1], "pair") == 0){
		/* challenge pair TOKEN CODE: the approval's, an assertion by one of the account's passkeys */
		c->op = Cnone;
		if((name = findsession(f[2], nil, nil)) == nil)
			return "no such session";
		qlock(&pairlk);
		if((p = findpair(f[3])) != nil && strcmp(p->name, name) == 0 && p->state == Pclaimed)
			strecpy(h, h+sizeof h, p->h);
		qunlock(&pairlk);
		if(h[0] == 0){
			free(name);
			return "no such pairing";
		}
		ids.s = strdup("");
		ids.n = 0;
		qlock(&statelk);
		eachcred(name, idone, &ids);
		qunlock(&statelk);
		wachallenge(c->chal, sizeof c->chal);
		c->op = Cpair;
		c->expires = time(0) + Chaltime;
		free(c->bound);
		c->bound = strdup(h);
		c->handle[0] = 0;
		keepchal(c);
		reply(c, "challenge %s%s", c->chal, ids.s);
		free(ids.s);
		free(name);
		return nil;
	}
	if(strcmp(f[0], "challenge") == 0 && nf == 3 && strcmp(f[1], "pairadd") == 0){
		/* challenge pairadd KEY: the new device's passkey, once approved */
		c->op = Cnone;
		qlock(&pairlk);
		if((p = findpairkey(f[2])) != nil && p->state == Papproved){
			strecpy(h, h+sizeof h, p->h);
			name = strdup(p->name);
		}
		qunlock(&pairlk);
		if(name == nil)
			return "no such pairing";
		qlock(&statelk);
		acct = account(name);
		s = strdup(acct && aval(acct, "handle") ? aval(acct, "handle") : "-");
		freeattrs(acct);
		/* its passkeys' ids: the page excludes them (excludeCredentials) */
		ids.s = strdup("");
		ids.n = 0;
		eachcred(name, idone, &ids);
		qunlock(&statelk);
		strecpy(c->handle, c->handle+sizeof c->handle, s);
		free(s);
		wachallenge(c->chal, sizeof c->chal);
		c->op = Cpairadd;
		c->expires = time(0) + Chaltime;
		free(c->bound);
		c->bound = strdup(h);
		keepchal(c);
		reply(c, "challenge %s %s %s%s", c->chal, name, c->handle, ids.s);
		free(ids.s);
		free(name);
		return nil;
	}
	if(strcmp(f[0], "pair") != 0)
		return "bad request";
	if(nf == 3 && strcmp(f[1], "new") == 0){
		/* pair new TOKEN: one at a time an account, a new one ends its earlier */
		if((name = findsession(f[2], nil, nil)) == nil)
			return "no such session";
		newtoken(code, sizeof code, h);
		qlock(&pairlk);
		for(;;){
			for(p = pairs; p != nil; p = p->next)
				if(strcmp(p->name, name) == 0)
					break;
			if(p == nil)
				break;
			droppair(p);
		}
		if(trimpairs() >= Maxpairs)
			why = "too many pairings";
		else{
			p = mallocz(sizeof *p, 1);
			strecpy(p->h, p->h+sizeof p->h, h);
			p->name = strdup(name);
			p->state = Pwait;
			p->expires = expires = time(0) + Pairtime;
			p->next = pairs;
			pairs = p;
		}
		qunlock(&pairlk);
		if(why == nil){
			alog("pair %s: begun", name);
			reply(c, "ok %s %ld", code, expires);
		}
		memset(code, 0, sizeof code);
		free(name);
		return why;
	}
	if(nf == 3 && strcmp(f[1], "claim") == 0){
		/* pair claim CODE: the new device's, once */
		qlock(&pairlk);
		if((p = findpair(f[2])) == nil || p->state != Pwait)
			why = "no such pairing";
		else{
			newtoken(key, sizeof key, p->kh);
			genrandom(b, sizeof b);
			snprint(p->confirm, sizeof p->confirm, "%06lud",
				((ulong)b[0]<<24 | (ulong)b[1]<<16 | (ulong)b[2]<<8 | b[3]) % 1000000UL);
			strecpy(conf, conf+sizeof conf, p->confirm);
			p->state = Pclaimed;
			name = strdup(p->name);
		}
		qunlock(&pairlk);
		if(why != nil)
			return why;
		alog("pair %s: claimed from %s", name, c->addr);
		reply(c, "ok %s %s %s", key, conf, name);
		memset(key, 0, sizeof key);
		free(name);
		return nil;
	}
	if(nf == 4 && (strcmp(f[1], "status") == 0 || strcmp(f[1], "cancel") == 0)){
		/* pair status|cancel TOKEN CODE: the account's device's */
		if((name = findsession(f[2], nil, nil)) == nil)
			return "no such session";
		qlock(&pairlk);
		p = findpair(f[3]);
		if(p != nil && strcmp(p->name, name) != 0)
			p = nil;
		if(strcmp(f[1], "cancel") == 0){
			if(p != nil)
				droppair(p);
			s = strdup("ok");
		}else if(p == nil)
			s = strdup("ok gone");
		else switch(p->state){
		case Pwait:
			s = strdup("ok wait");
			break;
		case Pclaimed:
			s = smprint("ok claimed %s", p->confirm);
			break;
		case Pdone:
			s = strdup("ok done");
			break;
		default:
			s = strdup("ok approved");
			break;
		}
		qunlock(&pairlk);
		if(strcmp(f[1], "cancel") == 0)
			alog("pair %s: cancelled", name);
		reply(c, "%s", s);
		free(s);
		free(name);
		return nil;
	}
	if(nf == 3 && strcmp(f[1], "wait") == 0){
		/* pair wait KEY: the new device's */
		qlock(&pairlk);
		p = findpairkey(f[2]);
		s = p == nil ? "ok gone" : p->state == Pclaimed ? "ok claimed" : "ok approved";
		qunlock(&pairlk);
		reply(c, "%s", s);
		return nil;
	}
	if(nf == 8 && strcmp(f[1], "approve") == 0){
		/* pair approve TOKEN CODE ID AUTH CLIENT SIG: by one of the account's passkeys, now */
		adoptchal(c, f[6], "webauthn.get");
		name = findsession(f[2], nil, nil);
		qlock(&pairlk);
		if(name != nil && (p = findpair(f[3])) != nil && strcmp(p->name, name) == 0 && p->state == Pclaimed)
			strecpy(h, h+sizeof h, p->h);
		qunlock(&pairlk);
		why = takechal(c, Cpair, h[0] ? h : "-");
		if(name == nil)
			why = "no such session";
		else if(why == nil && h[0] == 0)
			why = "no such pairing";
		if(why != nil){
			free(name);
			return why;
		}
		r = mallocz(sizeof *r, 1);
		if((why = ceremony(c, r, f[4], f[5], f[6], f[7], "webauthn.get", 0)) == nil){
			qlock(&statelk);
			why = assertion(r, f[4], "-", &who);
			qunlock(&statelk);
			if(why == nil && strcmp(who, name) != 0)
				why = "not this account's passkey";
		}
		free(r);
		if(why == nil){
			qlock(&pairlk);
			if((p = findpair(f[3])) != nil && strcmp(p->h, h) == 0 && p->state == Pclaimed)
				p->state = Papproved;
			else
				why = "the pairing has ended";
			qunlock(&pairlk);
		}
		if(why == nil){
			alog("pair %s: approved", name);
			reply(c, "ok");
		}
		free(who);
		free(name);
		return why;
	}
	if(nf == 7 && strcmp(f[1], "add") == 0){
		/* pair add KEY ID AUTH CLIENT LABEL: one approval, one passkey */
		adoptchal(c, f[5], "webauthn.create");
		qlock(&pairlk);
		if((p = findpairkey(f[2])) != nil && p->state == Papproved){
			p->state = Padding;
			strecpy(h, h+sizeof h, p->h);
			name = strdup(p->name);
		}
		qunlock(&pairlk);
		why = takechal(c, Cpairadd, h[0] ? h : "-");
		if(why == nil && name == nil)
			why = "no such pairing";
		lab = label(f[6]);
		if(why == nil && lab == nil)
			why = "bad label";
		r = mallocz(sizeof *r, 1);
		if(why == nil)
			why = ceremony(c, r, f[3], f[4], f[5], nil, "webauthn.create", 1);
		if(why == nil){
			qlock(&statelk);
			if(!accountactive(name) || !keyenabled(name))
				why = "the account is not enabled";
			else if(countcreds(name) >= Maxcreds)
				why = "too many passkeys";
			else{
				p0 = pathf("%s/creds/%s", dir, r->idh);
				s = credtext(name, f[3], r->w.pub, r->w.count, lab, r->w.flags, time(0), 0);
				i = writefile(p0, 1, s);
				free(s);
				free(p0);
				if(i < 0)
					why = "the passkey is known";
			}
			qunlock(&statelk);
		}
		if(name != nil){
			/* done, or the new device may try again while the pairing lasts */
			qlock(&pairlk);
			if((p = findpairkey(f[2])) != nil && p->state == Padding)
				p->state = why == nil ? Pdone : Papproved;
			qunlock(&pairlk);
		}
		if(why == nil){
			newsession(name, r->idh, tok, sizeof tok, &expires);
			alog("pair %s: passkey %s added", name, r->idh);
			reply(c, "ok %s %s %ld", tok, name, expires);
			memset(tok, 0, sizeof tok);
		}
		free(r);
		free(name);
		return why;
	}
	return "bad request";
}

static void
pagereq(Conn *c, char **f, int nf)
{
	char tok[64], idh[40], *why, *name, *s;
	long expires, created;
	uchar hb[Nhandle];
	Attr *inv, *acct;
	List ids;

	why = "bad request";
	name = nil;
	if(nf >= 2 && (strcmp(f[0], "pair") == 0
	|| strcmp(f[0], "challenge") == 0 && (strcmp(f[1], "pair") == 0 || strcmp(f[1], "pairadd") == 0))){
		if((why = pairreq(c, f, nf)) == nil)
			return;
		goto err;
	}
	if(nf == 2 && strcmp(f[0], "challenge") == 0 && strcmp(f[1], "login") == 0){
		wachallenge(c->chal, sizeof c->chal);
		c->op = Clogin;
		c->expires = time(0) + Chaltime;
		free(c->bound);
		c->bound = nil;
		keepchal(c);
		reply(c, "challenge %s", c->chal);
		return;
	}
	if(nf == 3 && strcmp(f[0], "challenge") == 0 && strcmp(f[1], "enroll") == 0){
		qlock(&statelk);
		c->op = Cnone;
		if((inv = invitation(f[2], idh, &why)) != nil){
			name = strdup(aval(inv, "name"));
			acct = account(name);
			if(acct != nil && aval(acct, "handle") != nil)
				strecpy(c->handle, c->handle+sizeof c->handle, aval(acct, "handle"));
			else{
				genrandom(hb, sizeof hb);
				wab64uenc(c->handle, sizeof c->handle, hb, sizeof hb);
			}
			freeattrs(acct);
			freeattrs(inv);
			wachallenge(c->chal, sizeof c->chal);
			c->op = Cenroll;
			c->expires = time(0) + Chaltime;
			free(c->bound);
			c->bound = strdup(idh);
			keepchal(c);
		}
		qunlock(&statelk);
		if(name == nil)
			goto err;
		reply(c, "challenge %s %s %s", c->chal, name, c->handle);
		free(name);
		return;
	}
	if(nf == 3 && strcmp(f[0], "challenge") == 0 && strcmp(f[1], "add") == 0){
		c->op = Cnone;
		if((name = findsession(f[2], &created, nil)) == nil){
			why = "no such session";
			goto err;
		}
		if(created + recent < time(0)){
			why = "sign in again first";
			goto err;
		}
		qlock(&statelk);
		acct = account(name);
		s = strdup(acct && aval(acct, "handle") ? aval(acct, "handle") : "-");
		freeattrs(acct);
		strecpy(c->handle, c->handle+sizeof c->handle, s);
		free(s);
		/* its passkeys' ids: the page excludes them (excludeCredentials) */
		ids.s = strdup("");
		ids.n = 0;
		eachcred(name, idone, &ids);
		qunlock(&statelk);
		wachallenge(c->chal, sizeof c->chal);
		c->op = Cadd;
		c->expires = time(0) + Chaltime;
		free(c->bound);
		c->bound = strdup(name);
		keepchal(c);
		reply(c, "challenge %s %s %s%s", c->chal, name, c->handle, ids.s);
		free(ids.s);
		free(name);
		return;
	}
	if(nf == 6 && strcmp(f[0], "enroll") == 0){
		if((why = enroll(c, f, tok, sizeof tok, &name, &expires)) != nil)
			goto err;
		reply(c, "ok %s %s %ld", tok, name, expires);
		free(name);
		memset(tok, 0, sizeof tok);
		return;
	}
	if(nf == 6 && strcmp(f[0], "login") == 0){
		if((why = login(c, f, tok, sizeof tok, &name, &expires)) != nil)
			goto err;
		alog("login %s", name);
		reply(c, "ok %s %s %ld", tok, name, expires);
		free(name);
		memset(tok, 0, sizeof tok);
		return;
	}
	if(nf == 6 && strcmp(f[0], "add") == 0){
		if((why = addcred(c, f, idh)) != nil)
			goto err;
		reply(c, "ok %s", idh);
		return;
	}
	if(nf == 2 && strcmp(f[0], "session") == 0){
		if((name = findsession(f[1], nil, &expires)) == nil){
			why = "no such session";
			goto err;
		}
		reply(c, "ok %s %ld", name, expires);
		free(name);
		return;
	}
	if(nf == 2 && strcmp(f[0], "creds") == 0){
		if((name = findsession(f[1], nil, nil)) == nil){
			why = "no such session";
			goto err;
		}
		qlock(&statelk);
		s = listcreds(name);
		qunlock(&statelk);
		reply(c, "%s", s);
		free(s);
		free(name);
		return;
	}
	if(nf == 3 && strcmp(f[0], "revoke") == 0){
		if((name = findsession(f[1], nil, nil)) == nil){
			why = "no such session";
			goto err;
		}
		qlock(&statelk);
		why = revokecred(name, f[2], 0);
		qunlock(&statelk);
		if(why != nil)
			goto err;
		reply(c, "ok");
		free(name);
		return;
	}
	if(nf == 2 && strcmp(f[0], "logout") == 0){
		logoutsession(f[1]);
		reply(c, "ok");
		return;
	}
err:
	alog("%s: %q refused: %s", c->addr, nf > 0 ? f[0] : "-", why);
	free(name);
	sleep(1000);	/* slower guessing */
	reply(c, "error %s", why);
}

/* the host owner's requests */

static void
listinvite(char *h, Attr *a, void *arg)
{
	List *l;
	char *s;

	l = arg;
	s = smprint("%s\n%s %s %s %s", l->s, h, aval(a, "name") ? aval(a, "name") : "-",
		aval(a, "expires") ? aval(a, "expires") : "0", aval(a, "groups") ? aval(a, "groups") : "");
	free(l->s);
	l->s = s;
}

static void
eachfile(char *sub, void (*fn)(char*, Attr*, void*), void *arg)
{
	Dir *d;
	Attr *a;
	char *p;
	int fd, n, i;

	p = pathf("%s/%s", dir, sub);
	fd = open(p, OREAD);
	if(fd < 0){
		free(p);
		return;
	}
	while((n = dirread(fd, &d)) > 0){
		for(i = 0; i < n; i++){
			free(p);
			p = pathf("%s/%s/%s", dir, sub, d[i].name);
			if((a = readattrs(p)) != nil){
				fn(d[i].name, a, arg);
				freeattrs(a);
			}
		}
		free(d);
	}
	free(p);
	close(fd);
}

static void
listuser(char *name, Attr *a, void *arg)
{
	List *l;
	char *s;

	l = arg;
	s = smprint("%s\n%s %s %s %d", l->s, name, aval(a, "state") ? aval(a, "state") : "-",
		aval(a, "created") ? aval(a, "created") : "0", countcreds(name));
	free(l->s);
	l->s = s;
}

static void
pendinginvite(char *, Attr *a, void *arg)
{
	char *v;

	v = aval(a, "name");
	if(v != nil && strcmp(v, (char*)((void**)arg)[0]) == 0
	&& aval(a, "expires") != nil && strtol(aval(a, "expires"), nil, 10) > time(0))
		((void**)arg)[1] = (void*)1;
}

static char*
mkinvite(char **f, int nf, char *code, int ncode)
{
	uchar b[Ncode];
	char h[40], *why, *p, *s, *gs, *t;
	void *arg[2];
	long secs;
	int pending, i;

	if((why = newnameok(f[1], &pending)) != nil)
		return why;
	arg[0] = f[1];
	arg[1] = nil;
	eachfile("invites", pendinginvite, arg);
	if(arg[1] != nil)
		return "an invitation for that name is open";
	secs = strtol(f[2], &t, 10);
	if(*t != 0 || secs < 60 || secs > 30*24*60*60)
		return "bad time (60 s to 30 days)";
	gs = strdup("");
	for(i = 3; i < nf; i++){
		if((why = groupok(f[i])) != nil){
			free(gs);
			return why;
		}
		s = smprint("%s%s%s", gs, *gs ? " " : "", f[i]);
		free(gs);
		gs = s;
	}
	genrandom(b, sizeof b);
	wab64uenc(code, ncode, b, sizeof b);
	memset(b, 0, sizeof b);
	hashname(h, (uchar*)code, strlen(code));
	p = pathf("%s/invites/%s", dir, h);
	s = smprint("name %s\nexpires %ld\ngroups %s\ncreated %ld\n", f[1], time(0)+secs, gs, time(0));
	i = writefile(p, 1, s);
	free(s);
	free(p);
	free(gs);
	if(i < 0)
		return "can not keep the invitation";
	alog("invite %s: %s", f[1], h);
	return nil;
}

static char*
setstate(char *name, char *state)
{
	Attr *a;
	char *p, *s, *v;
	int i;

	if((a = account(name)) == nil || (v = aval(a, "state")) == nil || strcmp(v, "pending") == 0){
		freeattrs(a);
		return "no such account";
	}
	p = pathf("%s/users/%s", dir, name);
	s = smprint("state %s\nhandle %s\ncreated %s\n", state,
		aval(a, "handle") ? aval(a, "handle") : "-", aval(a, "created") ? aval(a, "created") : "0");
	i = writefile(p, 0, s);
	free(s);
	free(p);
	freeattrs(a);
	/* a disabled account's sessions end even if keyfs fails below */
	if(strcmp(state, "active") != 0)
		endsessions(name, nil);
	if(i < 0)
		return "can not keep the account";
	if(keyfswrite(name, "status", strcmp(state, "active") == 0 ? "ok" : "disabled",
	   strcmp(state, "active") == 0 ? 2 : 8) < 0)
		return "keyfs";
	alog("%s %s", state, name);
	return nil;
}

static char*
groupsof(char *name)
{
	Biobuf *b;
	char *l, *f[4], *m[256], *s, *t;
	int i, n;

	if((b = Bopen(admusers, OREAD)) == nil)
		return strdup("error no users file");
	s = strdup("ok");
	while((l = Brdstr(b, '\n', 1)) != nil){
		if(getfields(l, f, nelem(f), 0, ":") == 4){
			n = getfields(f[3], m, nelem(m), 0, ",");
			for(i = 0; i < n; i++)
				if(strcmp(m[i], name) == 0){
					t = smprint("%s %s", s, f[1]);
					free(s);
					s = t;
				}
		}
		free(l);
	}
	Bterm(b);
	return s;
}

static void
adminreq(Conn *c, char **f, int nf)
{
	char code[64], buf[64], *why, *s, *name;
	uchar aes[AESKEYLEN];
	List l;
	Sess *ss;
	Attr *a;
	int n;

	why = "bad request";
	if(nf >= 3 && strcmp(f[0], "invite") == 0){
		qlock(&statelk);
		why = mkinvite(f, nf, code, sizeof code);
		qunlock(&statelk);
		if(why == nil){
			reply(c, "ok %s", code);
			memset(code, 0, sizeof code);
			return;
		}
	}else if(nf == 1 && strcmp(f[0], "invites") == 0){
		l.s = strdup("ok");
		qlock(&statelk);
		eachfile("invites", listinvite, &l);
		qunlock(&statelk);
		reply(c, "%s", l.s);
		free(l.s);
		return;
	}else if(nf == 2 && strcmp(f[0], "uninvite") == 0){
		why = "no such invitation";
		if(ishash(f[1])){
			s = pathf("%s/invites/%s", dir, f[1]);
			qlock(&statelk);
			if(remove(s) == 0)
				why = nil;
			qunlock(&statelk);
			free(s);
		}
	}else if(nf == 1 && strcmp(f[0], "users") == 0){
		l.s = strdup("ok");
		qlock(&statelk);
		eachfile("users", listuser, &l);
		qunlock(&statelk);
		reply(c, "%s", l.s);
		free(l.s);
		return;
	}else if(nf == 2 && strcmp(f[0], "creds") == 0){
		qlock(&statelk);
		s = listcreds(f[1]);
		qunlock(&statelk);
		reply(c, "%s", s);
		free(s);
		return;
	}else if(nf == 3 && strcmp(f[0], "revoke") == 0){
		qlock(&statelk);
		why = revokecred(f[1], f[2], 1);
		qunlock(&statelk);
	}else if(nf == 2 && (strcmp(f[0], "disable") == 0 || strcmp(f[0], "enable") == 0)){
		qlock(&statelk);
		why = setstate(f[1], strcmp(f[0], "enable") == 0 ? "active" : "disabled");
		qunlock(&statelk);
	}else if(nf == 2 && strcmp(f[0], "groups") == 0){
		s = groupsof(f[1]);
		reply(c, "%s", s);
		free(s);
		return;
	}else if(nf == 3 && strcmp(f[0], "group") == 0 && (f[2][0] == '+' || f[2][0] == '-')){
		qlock(&statelk);
		if((a = account(f[1])) == nil || aval(a, "state") == nil || strcmp(aval(a, "state"), "pending") == 0)
			why = "no such account";
		else
			why = setgroup(f[1], f[2]+1, f[2][0] == '+');
		freeattrs(a);
		qunlock(&statelk);
		if(why == nil)
			alog("group %s %s", f[1], f[2]);
	}else if(nf == 1 && strcmp(f[0], "sessions") == 0){
		n = 0;
		qlock(&sesslk);
		for(ss = sessions; ss != nil; ss = ss->next)
			if(ss->expires > time(0))
				n++;
		qunlock(&sesslk);
		reply(c, "ok %d", n);
		return;
	}else if(nf == 2 && strcmp(f[0], "logout") == 0){
		endsessions(f[1], nil);
		why = nil;
	}else if(nf == 2 && strcmp(f[0], "asuser") == 0){
		why = "no such session";
		if((name = findsession(f[1], nil, nil)) != nil){
			why = "the account is not enabled";
			if(accountactive(name) && keyenabled(name) && keyfsread(name, "status", buf, sizeof buf) != nil){
				s = pathf("%s/%s/aeskey", keys, name);
				n = open(s, OREAD);
				free(s);
				if(n >= 0 && read(n, aes, sizeof aes) == sizeof aes){
					enc16(buf, sizeof buf, aes, sizeof aes);
					memset(aes, 0, sizeof aes);
					close(n);
					alog("asuser %s", name);
					reply(c, "ok %s %s %s", name, authdom, buf);
					memset(buf, 0, sizeof buf);
					free(name);
					return;
				}
				if(n >= 0)
					close(n);
			}
			free(name);
		}
	}
	if(why != nil){
		alog("admin: %s refused: %s", nf > 0 ? f[0] : "-", why);
		reply(c, "error %s", why);
	}else
		reply(c, "ok");
}

static int pipenote(void*, char*);

static void
connproc(void *a)
{
	char buf[Maxmsg+1], *f[40];
	Conn *c;
	int n, nf, nreq, gone;

	c = a;
	threadsetname("accountd %s %s", c->admin ? "admin" : "account", c->addr);
	threadnotify(pipenote, 1);	/* a proc's own: libthread's handlers are per proc */
	gone = 0;
	for(nreq = 0; c->admin || nreq < Maxreq; nreq++){
		n = read(c->fd, buf, sizeof buf - 1);
		if(n <= 0 || n == 1 && buf[0] == 'X')
			gone = 1;	/* the other end closed or ended it: nobody to tell */
		if(n <= 1 || buf[0] != 'T')
			break;
		buf[n] = 0;
		nf = tokenize(buf+1, f, nelem(f));
		if(nf <= 0)
			continue;
		if(c->admin)
			adminreq(c, f, nf);
		else
			pagereq(c, f, nf);
	}
	if(!gone)
		write(c->fd, "X", 1);
	close(c->fd);
	free(c->addr);
	free(c->bound);
	memset(c, 0, sizeof *c);
	free(c);
}

/* a connection's other end gone while answering it: that connection's end, not its proc's */
static int
pipenote(void*, char *note)
{
	return strstr(note, "write on closed pipe") != nil;
}

static void
usage(void)
{
	fprint(2, "usage: auth/accountd -r rpid -o origin... -d authdom -c fscons [-D dir] [-k keys] [-u usr] [-a users] [-g group]... [-s srvname] [-t ttl] [-R recent] [-l]\n");
	threadexitsall("usage");
}

static void
mkdirs(void)
{
	char *sub[] = { "", "/invites", "/users", "/creds" };
	char *p;
	int i, fd;

	for(i = 0; i < nelem(sub); i++){
		p = pathf("%s%s", dir, sub[i]);
		if(!exists(p)){
			/* another accountd (its own -s) may make it at the same time */
			if((fd = create(p, OREAD|OEXCL, DMDIR|0700)) >= 0)
				close(fd);
			else if(!exists(p))
				sysfatal("%s: %r", p);
		}
		free(p);
	}
}

void
threadmain(int argc, char **argv)
{
	char buf[256], *f[5], *p;
	int ctl[2], fd, n, admin;
	Dir *d, *d2;
	Conn *c;

	quotefmtinstall();
	ARGBEGIN{
	case 'r':
		rpid = EARGF(usage());
		break;
	case 'o':
		if(norigins == nelem(origins))
			usage();
		origins[norigins++] = EARGF(usage());
		break;
	case 'd':
		authdom = EARGF(usage());
		break;
	case 'c':
		fscons = EARGF(usage());
		break;
	case 'D':
		dir = EARGF(usage());
		break;
	case 'k':
		keys = EARGF(usage());
		break;
	case 'u':
		usrdir = EARGF(usage());
		break;
	case 'a':
		admusers = EARGF(usage());
		break;
	case 'g':
		if(nallowed == nelem(allowed))
			usage();
		allowed[nallowed++] = EARGF(usage());
		break;
	case 's':
		srvname = EARGF(usage());
		break;
	case 't':
		ttl = strtol(EARGF(usage()), nil, 10);
		break;
	case 'R':
		recent = strtol(EARGF(usage()), nil, 10);
		break;
	case 'l':
		logerr = 1;
		break;
	default:
		usage();
	}ARGEND
	if(argc != 0 || rpid == nil || norigins == 0 || authdom == nil || fscons == nil || ttl < 60 || recent < 10)
		usage();
	if((fd = open("#c/hostowner", OREAD)) < 0 || (n = read(fd, hostowner, sizeof hostowner - 1)) <= 0)
		sysfatal("hostowner: %r");
	close(fd);
	hostowner[n] = 0;
	if(strcmp(getuser(), hostowner) != 0)
		sysfatal("not the host owner");
	mkdirs();

	/* the connections' ctl: "conn SRVNAME KIND ADDR" to /srv/srvname */
	if(pipe(ctl) < 0)
		sysfatal("pipe: %r");
	snprint(buf, sizeof buf, "/srv/%s", srvname);
	remove(buf);
	if((fd = create(buf, OWRITE|ORCLOSE, 0602)) < 0)
		sysfatal("%s: %r", buf);
	if(fprint(fd, "%d", ctl[0]) < 0)
		sysfatal("%s: %r", buf);
	close(ctl[0]);
	alog("started: rp %s", rpid);
	for(;;){
		if((n = read(ctl[1], buf, sizeof buf - 1)) <= 0)
			sysfatal("ctl: %r");
		buf[n] = 0;
		if(tokenize(buf, f, nelem(f)) != 4 || strcmp(f[0], "conn") != 0
		|| strcmp(f[2], "account") != 0 && strcmp(f[2], "admin") != 0
		|| strchr(f[1], '/') != nil || strncmp(f[1], srvname, strlen(srvname)) != 0 || f[1][strlen(srvname)] != '.'){
			alog("ctl: not a connection");
			continue;
		}
		p = smprint("/srv/%s", f[1]);
		admin = 0;
		d = nil;
		if(strcmp(f[2], "admin") == 0){
			/* only a pipe the host owner posted */
			if((d = dirstat(p)) == nil || strcmp(d->uid, hostowner) != 0 || (d->mode & 077) != 0){
				free(d);
				free(p);
				alog("ctl: an admin connection not the host owner's");
				continue;
			}
			admin = 1;
		}
		fd = open(p, ORDWR);
		if(fd >= 0 && admin){
			/*
			 * the same /srv entry before and after the open: one
			 * removed and posted again by someone else has another
			 * qid (srv's paths are not reused)
			 */
			d2 = dirstat(p);
			if(d2 == nil || d2->qid.path != d->qid.path || strcmp(d2->uid, hostowner) != 0){
				close(fd);
				fd = -1;
				werrstr("posted again while opened");
			}
			free(d2);
		}
		free(d);
		free(p);
		if(fd < 0){
			alog("%s: %s: %r", f[3], f[1]);
			continue;
		}
		c = mallocz(sizeof *c, 1);
		c->fd = fd;
		c->addr = strdup(f[3]);
		c->admin = admin;
		proccreate(connproc, c, Stack);
	}
}
