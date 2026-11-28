/*
 * ethernetd - the wasm32 machines' network on a server: a browser's
 * machine (its Ethernet card, etherwasm32.c) reaches this server's file
 * service, as its signed-in account's user, through this machine.
 * aux/etherws brings each WebSocket here (one per connection); this runs
 * once, as the host owner, from cpustart.
 *
 * A connection's first message (etherws's ctl write to /srv/ethernetd) is
 *	conn SRVNAME ether ADDR
 * SRVNAME a pipe in /srv (etherws's, mode 0660), ADDR the browser's
 * address (for the log).  On the pipe a message is a write: its first
 * byte T (a WebSocket text message) or B (a binary one, an Ethernet
 * frame), then its bytes; X alone, from here, ends the connection.
 *
 * The machine's page:
 *	-> session TOKEN	an auth/accountd session (README.md, "Accounts and passkeys")
 *	<- ok | error WHY
 *	then frames both ways.
 * The session is accountd's (-a: its srvname), asked as the host owner
 * asks (admin "session TOKEN": the account's name, active, not expired);
 * no passkeys or accounts of its own.  The session is network permission
 * and nothing else: every 60 seconds, and at once for a new connection,
 * accountd is asked again, and a session ended (logout, revocation,
 * expiry, a disabled account) ends the machine's connection and its file
 * service.  One connection a name: a new one takes the old one's place
 * and address (a page reloaded, a connection lost).
 *
 * The machine is a host of NET (-n, 100.64.0.0/24: not the 10.* or
 * 192.168.* this server's own services trust), the gateway its .1, which
 * answers its ARP, DHCP and ping.  Its frames must come from one MAC
 * address and its packets from its own address.
 *
 * File service: TCP to the gateway's port -f (564) reaches this server's
 * stack through a pkt interface; a call from a machine's address is
 * served by exportfs -r ROOT (-X, /) with exportfs -P PATTERN (-x,
 * required: the user's whole namespace would include /net, /srv and
 * /proc) as that machine's account: auth/asuser with the connection's
 * session (a genuine dp9ik exchange through the auth server, the user's
 * own namespace, the file server checking the user's permissions).  The
 * session token never leaves this process but to asuser's standard
 * input.  At most Nexport exports a machine; they end with its connection.
 *
 * Out (-e only): into this server's IP stack and out with its address
 * (9front's NAT on a default route, the default one's gateway and
 * interface): TCP to ports 80, 443 and 9418, DNS (UDP and TCP 53) to the
 * resolvers (-D), ICMP echo; never to this server, the private, shared,
 * link-local (the cloud's metadata), multicast or reserved networks; no
 * fragments, no IPv6.  Without -e nothing goes beyond this server's file
 * service.  Each machine has -R bytes a second each way (1 MB), 20 new
 * TCP connections a second, 2 GB a day a name; at most -m at once (8);
 * -i idle seconds end it (15 minutes).  The log (syslog ether, or -l:
 * standard error) has the connections, their bytes, the exports and each
 * new outward TCP connection's destination.
 *
 *	aux/ethernetd -x pattern [-a accountsrv] [-s srvname] [-n net] [-f port] [-X root]
 *		[-e] [-g gate ifc] [-D resolver]... [-m max] [-i idle] [-R rate] [-l] [-v]
 * (-v: each connection in the log too)
 */
#include <u.h>
#include <libc.h>
#include <bio.h>
#include <mp.h>
#include <libsec.h>
#include <thread.h>
#include <ip.h>

/* auth/accountd's connections (../accounts/conn.c) */
int	dialaccountd(char *srv);
void	hangupaccountd(int fd);
char*	askaccountd(int fd, char *req);

enum {
	Stack	= 64*1024,
	Maxmsg	= 8*1024,	/* a message: a text's longest */
	Authtime	= 120,	/* seconds for a page to answer */
	Recheck	= 60,	/* seconds between a session's checks */
	Ttl	= 60*60,	/* a DHCP lease's seconds */
	Nexport	= 4,	/* file service connections a machine */
	Burst	= 256*1024,
	Synrate	= 20,
	Nout	= 256,	/* frames waiting for a connection: more are dropped */
	Nlease	= 256,
	Etherhdr	= 14,
	Maxframe	= 1514,
};

typedef struct Bucket Bucket;
typedef struct Conn Conn;
typedef struct Day Day;
typedef struct Msg Msg;
typedef struct Net Net;

struct Bucket
{
	vlong	t;	/* ms */
	double	n;
};

struct Msg
{
	int	n;
	uchar	b[1];	/* T or B, then its bytes */
};

struct Conn
{
	int	fd;
	char	*addr;
	Channel	*out;	/* Msg* to the writer; nil ends it */
	char	tok[128];	/* its accountd session */
	char	*name;
	int	export[Nexport];	/* its file service's calls: their ctl fds+1 */
	char	exportdir[Nexport][40];
	int	lease;	/* its host on NET, 0 none */
	uchar	mac[6];
	int	hasmac;
	Bucket	up, down, syn;
	long	start;
	long	last;	/* a frame from it */
	vlong	bytes[2];	/* up, down */
	int	dead;	/* to be ended */
	int	ended;	/* its end sent to etherws */
	Conn	*next;	/* conns */
};

struct Day
{
	char	*name;
	long	day;
	vlong	bytes;
	Day	*next;
};

struct Net
{
	ulong	a, m;
};


static char *accountsrv = "accountd";
static char *exportroot = "/";
static char *exportpat;
static int fport = 564;
static int egress;
static char *srvname = "ethernetd";
static int logerr, verbose;
static int maxmach = 8;
static int idle = 15*60;	/* -i: seconds without a frame that end a machine's connection */
static double rate = 1000*1000;
static vlong daycap = 2000LL*1000*1000;
static ulong net = 0x64400000, mask = 0xffffff00;	/* 100.64.0.0/24 */
static uchar gwmac[6] = { 0x02, 0x00, 0x64, 0x00, 0x00, 0x01 };
static ulong resolvers[8];
static int nresolvers;
static ulong self[32];	/* this machine's addresses */
static int nself;
static char *gate, *gateifc;
static int netfd = -1;

static QLock lk;	/* the leases, connections and days */
static Conn *leases[Nlease];
static Conn *conns;	/* all of them, for the timer */
static Day *days;

static Net blocked[] = {
	0x00000000, 0xff000000,	/* this network */
	0x0a000000, 0xff000000,	/* private */
	0x64400000, 0xffc00000,	/* shared (carrier NAT; the machines') */
	0x7f000000, 0xff000000,	/* loopback */
	0xa9fe0000, 0xffff0000,	/* link-local: the cloud's metadata */
	0xac100000, 0xfff00000,	/* private */
	0xc0000000, 0xffffff00,	/* IETF */
	0xc0a80000, 0xffff0000,	/* private */
	0xc6120000, 0xfffe0000,	/* benchmarking */
	0xe0000000, 0xf0000000,	/* multicast */
	0xf0000000, 0xf0000000,	/* reserved, broadcast */
};

static void
elog(char *fmt, ...)
{
	char buf[512];
	va_list arg;

	va_start(arg, fmt);
	vsnprint(buf, sizeof buf, fmt, arg);
	va_end(arg);
	if(logerr)
		fprint(2, "ethernetd: %s\n", buf);
	else
		syslog(0, "ether", "%s", buf);
}

static int
take(Bucket *b, double r, double burst, double n)
{
	vlong now;

	now = nsec()/1000000;
	if(b->t == 0)
		b->n = burst;
	else
		b->n += (now - b->t) * r / 1000;
	if(b->n > burst)
		b->n = burst;
	b->t = now;
	if(b->n < n)
		return 0;
	b->n -= n;
	return 1;
}

/* n more bytes of name's today: 0 if past its day's; lk held */
static int
daycountl(char *name, vlong n)
{
	Day *d;
	long today;
	int ok;

	today = time(0) / (24*60*60);
	for(d = days; d != nil; d = d->next)
		if(strcmp(d->name, name) == 0)
			break;
	if(d == nil){
		d = mallocz(sizeof *d, 1);
		d->name = strdup(name);
		d->next = days;
		days = d;
	}
	if(d->day != today){
		d->day = today;
		d->bytes = 0;
	}
	ok = d->bytes + n <= daycap;
	if(ok)
		d->bytes += n;
	return ok;
}

static int
daycount(char *name, vlong n)
{
	int ok;

	qlock(&lk);
	ok = daycountl(name, n);
	qunlock(&lk);
	return ok;
}

static Msg*
mkmsg(int type, void *p, int n)
{
	Msg *m;

	m = malloc(sizeof *m + n);
	if(m == nil)
		return nil;
	m->b[0] = type;
	memmove(m->b+1, p, n);
	m->n = 1+n;
	return m;
}

static void
sendtext(Conn *c, char *fmt, ...)
{
	char buf[256];
	va_list arg;
	Msg *m;

	va_start(arg, fmt);
	vsnprint(buf, sizeof buf, fmt, arg);
	va_end(arg);
	if((m = mkmsg('T', buf, strlen(buf))) != nil)
		sendp(c->out, m);
}

/* a frame to the machine: dropped if its queue is full */
static void
sendframe(Conn *c, uchar *f, int n)
{
	Msg *m;

	if((m = mkmsg('B', f, n)) != nil && nbsendp(c->out, m) == 0)
		free(m);
}

/* the end of the connection: etherws closes the WebSocket (a message of its type byte alone) */
static void
sendend(Conn *c)
{
	Msg *m;

	if((m = mkmsg('X', nil, 0)) != nil)
		sendp(c->out, m);
}

/* the same, lk held: not waiting for the writer (the timer tries again) */
static void
endl(Conn *c)
{
	Msg *m;

	if(c->ended || (m = mkmsg('X', nil, 0)) == nil)
		return;
	if(nbsendp(c->out, m))
		c->ended = 1;
	else
		free(m);
}

/*
 * a machine's file service ended (lk held): its calls hung up, so their
 * exportfs (the account's, not the host owner's to signal) read the end
 */
static void
endexports(Conn *c)
{
	int i;

	for(i = 0; i < Nexport; i++)
		if(c->export[i] > 0){
			hangup(c->export[i]-1);
			close(c->export[i]-1);
			c->export[i] = 0;
		}
}

/* a file service call still connected: its status Established */
static int
exportalive(char *dir)
{
	char path[64], buf[32];
	int fd, n;

	snprint(path, sizeof path, "%s/status", dir);
	if((fd = open(path, OREAD)) < 0)
		return 0;
	n = read(fd, buf, sizeof buf - 1);
	close(fd);
	buf[n > 0 ? n : 0] = 0;
	return strncmp(buf, "Established", 11) == 0;
}

/*
 * What takes time, without notes (a proc's read is not interrupted): every
 * 5 seconds a connection that has not shown its session in Authtime, or a
 * machine idle past -i, is ended (etherws closes it, its reader sees the
 * end) with its file service
 */
static void
timerproc(void*)
{
	Conn *c;
	long now;

	threadsetname("ethernetd timer");
	for(;;){
		sleep(5*1000);
		now = time(0);
		qlock(&lk);
		for(c = conns; c != nil; c = c->next){
			if(!c->dead && c->lease == 0 && now - c->start > Authtime)
				c->dead = 1;
			else if(!c->dead && c->lease != 0 && now - c->last > idle){
				elog("%s: %s idle", c->addr, c->name);
				c->dead = 1;
				if(leases[c->lease] == c)
					leases[c->lease] = nil;
			}
			if(c->dead){
				endexports(c);
				endl(c);
			}
		}
		qunlock(&lk);
	}
}

/*
 * accountd's answer to the host owner's "session TOKEN": the session's
 * account name (malloced), nil if it has none (ended, expired, the
 * account disabled) or accountd did not answer
 */
static char*
session(char *tok)
{
	char *a, *f[4], *name, *req;
	int fd;

	if((fd = dialaccountd(accountsrv)) < 0){
		elog("accountd: %r");
		return nil;
	}
	req = smprint("session %s", tok);
	a = askaccountd(fd, req);
	hangupaccountd(fd);
	if(req != nil){
		memset(req, 0, strlen(req));	/* the session token */
		free(req);
	}
	if(a == nil){
		elog("accountd: %r");
		return nil;
	}
	name = nil;
	if(tokenize(a, f, nelem(f)) == 3 && strcmp(f[0], "ok") == 0)
		name = strdup(f[1]);
	free(a);
	return name;
}

/* askaccountd's alarm: its read interrupted, not the proc ended */
static int
alarmnote(void*, char *note)
{
	return strcmp(note, "alarm") == 0;
}

/* every Recheck seconds each machine's session again: one ended ends the machine and its file service */
static void
sessproc(void*)
{
	char tok[Nlease][128], *name;
	Conn *cs[Nlease], *c;
	int ls[Nlease], i, n;

	threadsetname("ethernetd sessions");
	threadnotify(alarmnote, 1);
	for(;;){
		sleep(Recheck*1000);
		n = 0;
		qlock(&lk);
		for(i = 2; i < Nlease; i++)
			if((c = leases[i]) != nil && !c->dead){
				cs[n] = c;
				ls[n] = i;
				strecpy(tok[n], tok[n]+sizeof tok[n], c->tok);
				n++;
			}
		qunlock(&lk);
		for(i = 0; i < n; i++){
			name = session(tok[i]);
			qlock(&lk);
			c = cs[i];
			/* still the lease's (so not freed): a connection gone since is not looked at */
			if(leases[ls[i]] == c && !c->dead && (name == nil || strcmp(name, c->name) != 0)){
				elog("%s: %s: the session ended", c->addr, c->name);
				c->dead = 1;
				leases[c->lease] = nil;
				endexports(c);
				endl(c);
			}
			qunlock(&lk);
			free(name);
			memset(tok[i], 0, sizeof tok[i]);
		}
	}
}

static void
writeproc(void *a)
{
	Conn *c;
	Msg *m;

	c = a;
	threadsetname("ethernetd writer");
	while((m = recvp(c->out)) != nil){
			if(write(c->fd, m->b, m->n) != m->n)
			c->dead = 1;
			free(m);
	}
	close(c->fd);
	while((m = nbrecvp(c->out)) != nil)
		free(m);
	chanfree(c->out);
	free(c->addr);
	free(c);
}

static ushort
cksum(uchar *p, int n, ulong s)
{
	while(n > 1){
		s += p[0]<<8 | p[1];
		p += 2;
		n -= 2;
	}
	if(n)
		s += p[0]<<8;
	while(s >> 16)
		s = (s & 0xffff) + (s >> 16);
	return ~s;
}

static int
inet(ulong a, Net *n, int nn)
{
	int i;

	for(i = 0; i < nn; i++)
		if((a & n[i].m) == n[i].a)
			return 1;
	return 0;
}

static int
isself(ulong a)
{
	int i;

	for(i = 0; i < nself; i++)
		if(self[i] == a)
			return 1;
	return 0;
}

static int
isresolver(ulong a)
{
	int i;

	for(i = 0; i < nresolvers; i++)
		if(resolvers[i] == a)
			return 1;
	return 0;
}

/* an IPv4 packet to the machine, from the gateway's address and mac */
static void
toframe(Conn *c, uchar *ip, int n)
{
	uchar f[Maxframe];

	if(n > Maxframe - Etherhdr)
		return;
	memmove(f, c->mac, 6);
	memmove(f+6, gwmac, 6);
	f[12] = 0x08;
	f[13] = 0x00;
	memmove(f+Etherhdr, ip, n);
	sendframe(c, f, Etherhdr+n);
}

static void
arp(Conn *c, uchar *f, int n)
{
	uchar r[60], *a;

	if(n < Etherhdr+28)
		return;
	a = f+Etherhdr;
	if(a[0] != 0 || a[1] != 1 || a[2] != 0x08 || a[3] != 0 || a[4] != 6 || a[5] != 4)
		return;
	if(a[6] != 0 || a[7] != 1 || nhgetl(a+24) != (net|1))
		return;	/* only who has the gateway */
	memset(r, 0, sizeof r);
	memmove(r, a+8, 6);
	memmove(r+6, gwmac, 6);
	r[12] = 0x08;
	r[13] = 0x06;
	memmove(r+14, a, 6);
	r[20] = 0;
	r[21] = 2;
	memmove(r+22, gwmac, 6);
	hnputl(r+28, net|1);
	memmove(r+32, a+8, 6);
	memmove(r+38, a+14, 4);
	sendframe(c, r, sizeof r);
}

/* the gateway's ping */
static void
echo(Conn *c, uchar *ip, int len)
{
	uchar r[Maxframe-Etherhdr];
	int hl;

	hl = (ip[0]&15)*4;
	if(len - hl < 8 || len > sizeof r)
		return;
	memmove(r, ip, len);
	memmove(r+12, ip+16, 4);
	memmove(r+16, ip+12, 4);
	r[8] = 64;
	r[10] = r[11] = 0;
	hnputs(r+10, cksum(r, hl, 0));
	r[hl] = 0;	/* echo reply */
	r[hl+2] = r[hl+3] = 0;
	hnputs(r+hl+2, cksum(r+hl, len-hl, 0));
	toframe(c, r, len);
}

static void
dhcp(Conn *c, uchar *ip, int len)
{
	uchar r[600], *b, *o, *u, *p;
	int hl, nb, i, type, reply, ul;
	ulong yi, want;

	hl = (ip[0]&15)*4;
	b = ip+hl+8;
	nb = len - hl - 8;
	if(nb < 240 || b[0] != 1 || memcmp(b+236, "\x63\x82\x53\x63", 4) != 0)
		return;
	type = 0;
	want = nhgetl(b+12);
	for(i = 240; i < nb && b[i] != 255; ){
		if(b[i] == 0){
			i++;
			continue;
		}
		if(i+2 > nb || i+2+b[i+1] > nb)
			break;
		if(b[i] == 53 && b[i+1] >= 1)
			type = b[i+2];
		if(b[i] == 50 && b[i+1] == 4)
			want = nhgetl(b+i+2);
		i += 2 + b[i+1];
	}
	yi = net | c->lease;
	if(type == 1)
		reply = 2;	/* offer */
	else if(type == 3)
		reply = want == yi || want == 0 ? 5 : 6;	/* ack, nak */
	else
		return;

	memset(r, 0, sizeof r);
	u = r+20;
	p = u+8;
	p[0] = 2;
	p[1] = 1;
	p[2] = 6;
	memmove(p+4, b+4, 4);	/* xid */
	memmove(p+10, b+10, 2);	/* flags */
	if(reply != 6)
		hnputl(p+16, yi);
	hnputl(p+20, net|1);
	memmove(p+28, b+28, 16);	/* chaddr */
	memmove(p+236, "\x63\x82\x53\x63", 4);
	o = p+240;
	*o++ = 53; *o++ = 1; *o++ = reply;
	*o++ = 54; *o++ = 4; hnputl(o, net|1); o += 4;
	if(reply != 6){
		*o++ = 51; *o++ = 4; hnputl(o, Ttl); o += 4;
		*o++ = 1; *o++ = 4; hnputl(o, mask); o += 4;
		*o++ = 3; *o++ = 4; hnputl(o, net|1); o += 4;
		if(nresolvers > 0){
			*o++ = 6;
			*o++ = 4*nresolvers;
			for(i = 0; i < nresolvers; i++, o += 4)
				hnputl(o, resolvers[i]);
		}
	}
	*o++ = 255;
	ul = o - u;
	hnputs(u, 67);
	hnputs(u+2, 68);
	hnputs(u+4, ul);
	r[0] = 0x45;
	hnputs(r+2, 20+ul);
	r[8] = 64;
	r[9] = 17;
	hnputl(r+12, net|1);
	hnputl(r+16, 0xffffffff);
	hnputs(r+10, cksum(r, 20, 0));
	/* the UDP checksum over its pseudo-header */
	{
		ulong s;

		s = (net|1)>>16;
		s += (net|1) & 0xffff;
		s += 0xffff + 0xffff + 17 + ul;
		i = cksum(u, ul, s);
		hnputs(u+6, i == 0 ? 0xffff : i);
	}
	{
		uchar f[Maxframe];

		memset(f, 0xff, 6);
		memmove(f+6, gwmac, 6);
		f[12] = 0x08;
		f[13] = 0x00;
		memmove(f+Etherhdr, r, 20+ul);
		sendframe(c, f, Etherhdr+20+ul);
	}
	hnputl(r, yi);
	elog("%s %V dhcp %s", c->name, r, reply == 2 ? "offer" : reply == 5 ? "ack" : "nak");
}

/* a frame from the machine */
static void
fromvm(Conn *c, uchar *f, int n)
{
	uchar *ip, *l4, d[4];
	int hl, len, proto, dport;
	ulong src, dst;

	if(n < Etherhdr || n > Maxframe || (f[6] & 1) != 0)
		return;
	if(!c->hasmac){
		memmove(c->mac, f+6, 6);
		c->hasmac = 1;
	}else if(memcmp(c->mac, f+6, 6) != 0)
		return;	/* one address a port */
	if(f[12] == 0x08 && f[13] == 0x06){
		arp(c, f, n);
		return;
	}
	if(f[12] != 0x08 || f[13] != 0x00)
		return;	/* IPv6 and the rest */
	ip = f+Etherhdr;
	n -= Etherhdr;
	if(n < 20 || (ip[0]>>4) != 4)
		return;
	hl = (ip[0]&15)*4;
	len = nhgets(ip+2);
	if(hl < 20 || len < hl || len > n)
		return;
	src = nhgetl(ip+12);
	dst = nhgetl(ip+16);
	proto = ip[9];
	if(proto == 17 && len - hl >= 8 && nhgets(ip+hl+2) == 67 && (dst == 0xffffffff || dst == (net|1))){
		dhcp(c, ip, len);
		return;
	}
	if(src != (net|c->lease))
		return;	/* not its own address */
	if(dst == (net|1)){
		if(proto == 1 && len - hl >= 8 && ip[hl] == 8)
			echo(c, ip, len);
		/* the file service: into this server's stack, its own address on the pkt interface */
		else if(proto == 6 && len - hl >= 20 && nhgets(ip+hl+2) == fport && (ip[6] & 0x3f) == 0 && ip[7] == 0
		&& take(&c->up, rate, Burst, len)){
			c->bytes[0] += len;
			write(netfd, ip, len);
		}
		return;
	}
	if(!egress)
		return;
	if((ip[6] & 0x3f) != 0 || ip[7] != 0)
		return;	/* a fragment */
	if(isself(dst) || inet(dst, blocked, nelem(blocked)))
		return;
	l4 = ip+hl;
	switch(proto){
	case 6:
		if(len - hl < 20)
			return;
		dport = nhgets(l4+2);
		if(dport != 80 && dport != 443 && dport != 9418 && !(dport == 53 && isresolver(dst)))
			return;
		if((l4[13] & 0x12) == 0x02){	/* SYN, not ACK: a new connection */
			if(!take(&c->syn, Synrate, Synrate, 1))
				return;
			hnputl(d, dst);
			elog("%s %V tcp %V!%d", c->name, ip+12, d, dport);
		}
		break;
	case 17:
		if(len - hl < 8 || nhgets(l4+2) != 53 || !isresolver(dst))
			return;
		break;
	case 1:
		if(len - hl < 8 || l4[0] != 8)
			return;
		break;
	default:
		return;
	}
	if(!take(&c->up, rate, Burst, len) || !daycount(c->name, len))
		return;
	c->bytes[0] += len;
	write(netfd, ip, len);
}

/* the network's packets to the machines */
static void
netproc(void*)
{
	uchar buf[8*1024];
	Conn *c;
	ulong dst;
	int n, len;

	threadsetname("ethernetd net");
	while((n = read(netfd, buf, sizeof buf)) > 0){
		if(n < 20 || (buf[0]>>4) != 4)
			continue;
		len = nhgets(buf+2);
		if(len > n || len < 20)
			continue;
		dst = nhgetl(buf+16);
		if((dst & mask) != net || (dst & ~mask) == 0 || (dst & ~mask) >= Nlease)
			continue;
		/* under lk: a connection is not let go while it is a lease */
		qlock(&lk);
		c = leases[dst & ~mask];
		if(c != nil && !c->dead && c->hasmac && take(&c->down, rate, Burst, len) && daycountl(c->name, len)){
			c->bytes[1] += len;
			toframe(c, buf, len);
		}
		qunlock(&lk);
	}
	sysfatal("the network's interface: %r");
}

static void
etherconn(Conn *c)
{
	uchar buf[Maxmsg+1];
	char *f[3], *name;
	int n, i, l;
	Conn *old;

	n = read(c->fd, buf, sizeof buf - 1);
	if(n <= 1 || buf[0] != 'T')
		return;
	buf[n] = 0;
	if(tokenize((char*)buf+1, f, nelem(f)) != 2 || strcmp(f[0], "session") != 0 || strlen(f[1]) >= sizeof c->tok){
		sendtext(c, "error bad request");
		return;
	}
	if((name = session(f[1])) == nil){
		elog("%s: no such session", c->addr);
		sleep(1000);	/* slower guessing */
		sendtext(c, "error no such session");
		return;
	}
	strecpy(c->tok, c->tok+sizeof c->tok, f[1]);
	memset(buf, 0, n);
	qlock(&lk);
	/* one machine a name: a new connection takes the old one's place and address, the old one ended */
	l = 0;
	for(i = 2; i < Nlease; i++)
		if((old = leases[i]) != nil && strcmp(old->name, name) == 0){
			old->dead = 1;
			leases[i] = nil;
			endexports(old);
			endl(old);
			l = i;
			break;
		}
	if(l == 0){
		n = 0;
		for(i = 2; i < Nlease; i++)
			if(leases[i] != nil)
				n++;
		if(n < maxmach)
			for(i = 2; i < Nlease && i < (~mask & 0xff); i++)
				if(leases[i] == nil){
					l = i;
					break;
				}
	}
	if(l == 0){
		qunlock(&lk);
		elog("%s: %s: no room", c->addr, name);
		free(name);
		sendtext(c, "error the network is full; try later");
		return;
	}
	c->name = name;
	c->lease = l;
	c->last = time(0);
	leases[l] = c;
	qunlock(&lk);
	hnputl(buf, net|l);
	elog("%s: %s up as %V", c->addr, c->name, buf);
	sendtext(c, "ok");

	/* until the page goes, or the timer or another connection ends it */
	while((n = read(c->fd, buf, sizeof buf)) > 0 && !c->dead)
		if(buf[0] == 'B'){
			c->last = time(0);
			fromvm(c, buf+1, n-1);
		}
	qlock(&lk);
	if(leases[c->lease] == c)
		leases[c->lease] = nil;
	c->dead = 1;
	endexports(c);
	qunlock(&lk);
	elog("%s: %s down, %lld bytes up, %lld down", c->addr, c->name, c->bytes[0], c->bytes[1]);
}

static void
connproc(void *a)
{
	Conn *c, **l;

	c = a;
	threadsetname("ethernetd ether %s", c->addr);
	threadnotify(alarmnote, 1);
	etherconn(c);
	qlock(&lk);
	for(l = &conns; *l != nil; l = &(*l)->next)
		if(*l == c){
			*l = c->next;
			break;
		}
	qunlock(&lk);
	free(c->name);
	c->name = nil;
	memset(c->tok, 0, sizeof c->tok);
	if(!c->ended)
		sendend(c);
	sendp(c->out, nil);	/* the writer ends it */
	threadnotify(alarmnote, 0);	/* libthread's table has room for 32 procs' handlers */
}

/* this machine's addresses, which the machines may not reach */
static void
readself(void)
{
	Biobuf *b;
	char *l, *f[4];
	uchar ip[IPaddrlen];

	if((b = Bopen("/net/ipselftab", OREAD)) == nil)
		return;
	while((l = Brdstr(b, '\n', 1)) != nil){
		if(tokenize(l, f, nelem(f)) >= 1 && parseip(ip, f[0]) != -1 && isv4(ip) && nself < nelem(self))
			self[nself++] = nhgetl(ip+IPv4off);
		free(l);
	}
	Bterm(b);
}

/* the default route's gateway and interface, for the machines' route out */
static void
defaultroute(void)
{
	Biobuf *b;
	char *l, *f[10];

	if(gate != nil)
		return;
	if((b = Bopen("/net/iproute", OREAD)) == nil)
		sysfatal("/net/iproute: %r");
	while((l = Brdstr(b, '\n', 1)) != nil){
		if(tokenize(l, f, nelem(f)) >= 6 && strcmp(f[0], "0.0.0.0") == 0 && strcmp(f[1], "/96") == 0 && f[3][0] == '4'){
			gate = strdup(f[2]);
			gateifc = strdup(f[5]);
			free(l);
			break;
		}
		free(l);
	}
	Bterm(b);
	if(gate == nil)
		sysfatal("no default route: -g gate ifc");
}

/* routes with NET as their source (an earlier ethernetd's: 2026-10-08's cut the tunnel) removed */
static void
oldroutes(int rfd)
{
	Biobuf *b;
	char *l, *f[10];
	uchar a[4];
	char src[32];

	hnputl(a, net);
	snprint(src, sizeof src, "%V", a);
	if((b = Bopen("/net/iproute", OREAD)) == nil)
		return;
	while((l = Brdstr(b, '\n', 1)) != nil){
		if(tokenize(l, f, nelem(f)) == 8 && strcmp(f[6], src) == 0 && f[3][0] == '4'
		&& fprint(rfd, "del %s %s %s %s %s %s %s", f[0], f[1], f[2], f[3], f[5], f[6], f[7]) >= 0)
			elog("an old route removed: %s %s via %s from %s %s", f[0], f[1], f[2], f[6], f[7]);
		free(l);
	}
	Bterm(b);
}

static void
netsetup(void)
{
	char buf[32], path[64];
	uchar a[4], m[4], r[4];
	int cfd, n, rfd;

	if(egress)
		defaultroute();
	if((cfd = open("/net/ipifc/clone", ORDWR)) < 0)
		sysfatal("/net/ipifc/clone: %r");
	if((n = read(cfd, buf, sizeof buf - 1)) <= 0)
		sysfatal("/net/ipifc/clone: %r");
	buf[n] = 0;
	if(fprint(cfd, "bind pkt") < 0)
		sysfatal("bind pkt: %r");
	hnputl(a, net|1);
	hnputl(m, mask);
	hnputl(r, net);
	if(fprint(cfd, "add %V %V %V 1500", a, m, r) < 0)
		sysfatal("add: %r");
	if(egress && fprint(cfd, "iprouting 1") < 0)
		sysfatal("iprouting: %r");
	snprint(path, sizeof path, "/net/ipifc/%d/data", atoi(buf));
	if((netfd = open(path, ORDWR)) < 0)
		sysfatal("%s: %r", path);
	if(!egress){
		/* cfd stays open: the interface is gone when it is closed */
		elog("up: %V/%d, the file service only", a, 24);
		return;
	}
	/*
	 * the machines' route out: the default one's gateway and interface,
	 * translated (NAT: only what is forwarded is, never this machine's own).
	 * Not source specific (for 100.64.0.0/24 only): a lookup without a
	 * source - a WireGuard tunnel's outer packets (Plan2001's devwg) - takes
	 * any source specific route and a source from its range, and with this
	 * one the server's tunnel answered from 100.64.0.x, into nowhere
	 */
	if((rfd = open("/net/iproute", OWRITE)) < 0)
		sysfatal("/net/iproute: %r");
	oldroutes(rfd);
	if(fprint(rfd, "add 0.0.0.0 0.0.0.0 %s 4t %s 0.0.0.0 0.0.0.0", gate, gateifc) < 0)
		sysfatal("the route out: %r");
	close(rfd);
	/* cfd stays open: the interface is gone when it is closed */
	elog("up: %V/%d via %s on %s", a, 24, gate, gateifc);
}

static void
usage(void)
{
	fprint(2, "usage: aux/ethernetd -x pattern [-a accountsrv] [-s srvname] [-n net] [-f port] [-X root] [-e] [-g gate ifc] [-D resolver]... [-m max] [-i idle] [-R rate] [-l] [-v]\n");
	threadexitsall("usage");
}

/*
 * A call to the file service: the machine its address names, then
 * exportfs as its account (auth/asuser with its session on standard
 * input), the call on fd 3.  A machine that is not up, or has Nexport
 * already, is refused.
 */
static void
exportcall(char *adir)
{
	char ldir[40], path[64], raddr[64], *p, *tok;
	int lcfd, dfd, l, i, slot, pid, pfd[2];
	uchar ip[IPaddrlen];
	Conn *c;

	if((lcfd = listen(adir, ldir)) < 0)
		sysfatal("listen: %r");
	snprint(path, sizeof path, "%s/remote", ldir);
	raddr[0] = 0;
	if((dfd = open(path, OREAD)) >= 0){
		i = read(dfd, raddr, sizeof raddr - 1);
		raddr[i > 0 ? i : 0] = 0;
		close(dfd);
	}
	if((p = strchr(raddr, '!')) != nil)
		*p = 0;
	l = 0;
	if(parseip(ip, raddr) != -1 && isv4(ip) && (nhgetl(ip+IPv4off) & mask) == net)
		l = nhgetl(ip+IPv4off) & ~mask;
	tok = nil;
	slot = -1;
	qlock(&lk);
	if(l >= 2 && l < Nlease && (c = leases[l]) != nil && !c->dead){
		for(i = 0; i < Nexport; i++){
			if(c->export[i] > 0 && !exportalive(c->exportdir[i])){
				close(c->export[i]-1);
				c->export[i] = 0;
			}
			if(c->export[i] <= 0){
				slot = i;
				break;
			}
		}
		if(slot >= 0)
			tok = smprint("%s\n", c->tok);
	}
	if(tok == nil){
		qunlock(&lk);
		elog("file service: %s refused", raddr);
		hangup(lcfd);
		close(lcfd);
		return;
	}
	if((dfd = accept(lcfd, ldir)) < 0 || pipe(pfd) < 0){
		qunlock(&lk);
		elog("file service: %s: %r", raddr);
		close(lcfd);
		free(tok);
		return;
	}
	switch(pid = rfork(RFPROC|RFFDG|RFNOTEG|RFNOWAIT|RFREND)){
	case -1:
		elog("file service: fork: %r");
		break;
	case 0:
		dup(pfd[0], 0);
		dup(dfd, 3);
		/* nothing else of this process's: the other machines' calls, the pkt interface, the srv pipes */
		for(i = 4; i < 1024; i++)
			close(i);
		/* exportfs serves fds 0 and 1: the call's, once asuser has read the session from 0 */
		p = smprint("exec /bin/exportfs -r %q%s%q <[0=3] >[1=3] >[2]/dev/null",
			exportroot, exportpat != nil ? " -P " : "", exportpat != nil ? exportpat : "");
		execl("/bin/auth/asuser", "asuser", "/bin/rc", "-c", p, nil);
		_exits("exec");
	default:
		/* the call's ctl kept: hung up when the machine goes */
		c->export[slot] = lcfd+1;
		strecpy(c->exportdir[slot], c->exportdir[slot]+sizeof c->exportdir[slot], ldir);
		lcfd = -1;
		elog("%s: %s: file service %d", raddr, c->name, pid);
	}
	qunlock(&lk);
	close(pfd[0]);
	write(pfd[1], tok, strlen(tok));
	close(pfd[1]);
	memset(tok, 0, strlen(tok));
	free(tok);
	close(dfd);
	if(lcfd >= 0)
		close(lcfd);
}

/* the file service's calls, on the gateway's address of this server's stack */
static void
exportproc(void*)
{
	char addr[64], adir[40];
	uchar a[4];
	threadsetname("ethernetd file service");
	hnputl(a, net|1);
	snprint(addr, sizeof addr, "tcp!%V!%d", a, fport);
	if(announce(addr, adir) < 0)	/* its ctl stays open: the announcement */
		sysfatal("announce %s: %r", addr);
	elog("file service: %s", addr);
	for(;;)
		exportcall(adir);
}

static void
parsenet(char *s, ulong *a, ulong *m)
{
	uchar ip[IPaddrlen];
	char *p;
	int len;

	len = 32;
	if((p = strchr(s, '/')) != nil){
		*p++ = 0;
		len = atoi(p);
	}
	if(parseip(ip, s) == -1 || !isv4(ip) || len < 0 || len > 32)
		usage();
	*m = len == 0 ? 0 : ~0UL << (32-len);
	*a = nhgetl(ip+IPv4off) & *m;
}

void
threadmain(int argc, char **argv)
{
	char buf[256], *f[5], *p;
	int ctl[2], fd, n;
	uchar ip[IPaddrlen];
	Conn *c;

	quotefmtinstall();
	fmtinstall('V', eipfmt);
	fmtinstall('I', eipfmt);
	ARGBEGIN{
	case 'a':
		accountsrv = EARGF(usage());
		break;
	case 'f':
		fport = atoi(EARGF(usage()));
		break;
	case 'X':
		exportroot = EARGF(usage());
		break;
	case 'x':
		exportpat = EARGF(usage());
		break;
	case 'e':
		egress = 1;
		break;
	case 's':
		srvname = EARGF(usage());
		break;
	case 'n':
		parsenet(EARGF(usage()), &net, &mask);
		if((~mask & 0xff) < 3)
			usage();
		break;
	case 'g':
		gate = EARGF(usage());
		gateifc = EARGF(usage());
		break;
	case 'D':
		if(nresolvers == nelem(resolvers) || parseip(ip, EARGF(usage())) == -1 || !isv4(ip))
			usage();
		resolvers[nresolvers++] = nhgetl(ip+IPv4off);
		break;
	case 'm':
		maxmach = atoi(EARGF(usage()));
		break;
	case 'R':
		rate = atof(EARGF(usage()));
		break;
	case 'l':
		logerr = 1;
		break;
	case 'i':
		idle = atoi(EARGF(usage()));
		break;
	case 'v':
		verbose = 1;
		break;
	default:
		usage();
	}ARGEND
	/* without a pattern exportfs would export the user's whole namespace, /net included */
	if(argc != 0 || fport <= 0 || fport >= 65536 || exportpat == nil)
		usage();
	if(nresolvers == 0){
		parseip(ip, "9.9.9.9");
		resolvers[nresolvers++] = nhgetl(ip+IPv4off);
	}
	readself();
	netsetup();

	/* the connections' ctl: etherws writes "conn SRVNAME KIND ADDR" to /srv/srvname */
	if(pipe(ctl) < 0)
		sysfatal("pipe: %r");
	snprint(buf, sizeof buf, "/srv/%s", srvname);
	remove(buf);
	if((fd = create(buf, OWRITE|ORCLOSE, 0602)) < 0)
		sysfatal("%s: %r", buf);
	if(fprint(fd, "%d", ctl[0]) < 0)
		sysfatal("%s: %r", buf);
	close(ctl[0]);
	proccreate(netproc, nil, Stack);
	proccreate(timerproc, nil, Stack);
	proccreate(sessproc, nil, Stack);
	proccreate(exportproc, nil, Stack);
	for(;;){
		if((n = read(ctl[1], buf, sizeof buf - 1)) <= 0)
			sysfatal("ctl: %r");
		buf[n] = 0;
		if(tokenize(buf, f, nelem(f)) != 4 || strcmp(f[0], "conn") != 0
		|| strcmp(f[2], "ether") != 0
		|| strchr(f[1], '/') != nil || strncmp(f[1], srvname, strlen(srvname)) != 0){
			elog("ctl: not a connection");
			continue;
		}
		p = smprint("/srv/%s", f[1]);
		fd = open(p, ORDWR);
		free(p);
		if(fd < 0){
			elog("%s: %s: %r", f[3], f[1]);
			continue;
		}
		if(verbose)
			elog("%s: %s", f[3], f[2]);
		c = mallocz(sizeof *c, 1);
		c->fd = fd;
		c->addr = strdup(f[3]);
		c->out = chancreate(sizeof(Msg*), Nout);
		c->start = time(0);
		qlock(&lk);
		c->next = conns;
		conns = c;
		qunlock(&lk);
		proccreate(writeproc, c, Stack);
		proccreate(connproc, c, Stack);
	}
}
