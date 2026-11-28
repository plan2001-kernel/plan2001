/*
 * jsio - sivun (JavaScriptin) tiedosto-operaatiot tämän koneen
 * nimiavaruudessa, #j/js:n kautta (sys/src/9/wasm32/devjs.c).  Sivu ei
 * toteuta omaa tiedostojärjestelmää eikä 9P:tä: se pyytää, ja natiivi
 * ohjelma tekee tavalliset järjestelmäkutsut (open, pread, create,
 * dirfwstat, bind, mount ...); etätiedostot kulkevat ytimen oman 9P:n ja
 * verkon kautta.  Sivun JavaScript-asiakas: jsfs.js.
 *
 * Viesti on yksi kirjoitus kumpaankin suuntaan.  Pyyntö:
 *	TAG OP ARG...\n[DATA]
 * vastaus:
 *	TAG ok [ARVO...]\n[DATA]	tai	TAG error SYY\n
 * Sanat lainataan kuten rc:ssä (tokenize, %q).  TAG on sivun numero,
 * jolla se yhdistää vastauksen pyyntöön; pyynnöt suoritetaan
 * rinnakkain (Nwork prosessia), joten vastaukset voivat tulla eri
 * järjestyksessä.  Datan enimmäiskoko on Maxio tavua viestiä kohden.
 *
 *	stat PATH | fstat FID		<- ok\nDIR
 *	open PATH MODE			<- ok FID IOUNIT
 *	create PATH MODE PERM		<- ok FID		(PERM numerona, DMDIR mukaan lukien)
 *	read FID OFFSET COUNT		<- ok N\nDATA
 *	write FID OFFSET\nDATA		<- ok N
 *	dirread FID			<- ok N\nDIR...	(N 0: hakemisto luettu loppuun)
 *	close FID			<- ok
 *	get PATH OFFSET COUNT		<- ok N LENGTH\nDATA	(open, pread, close yhdellä pyynnöllä)
 *	remove PATH			<- ok
 *	wstat PATH K=V... | fwstat FID K=V...	(name, mode, gid, length, mtime) <- ok
 *	bind NEW OLD FLAGS		<- ok		(FLAGS: MREPL 0, MBEFORE 1, MAFTER 2, MCREATE 4)
 *	mount ADDR OLD FLAGS [SPEC]	<- ok		(dial ADDR, 9P ilman tunnistusta: palvelin on jo
 *					   tunnistanut yhteyden, esim. ethernetd:n istunto)
 *	unmount NEW|- OLD		<- ok
 *	ping				<- ok
 *
 * DIR on yksi rivi: NAME QIDPATH QIDVERS QIDTYPE MODE LENGTH MTIME ATIME UID GID MUID
 * (QIDPATH heksana, muut desimaaleina, nimet lainattuina).
 *
 *	aux/jsio [-f #j/js] [-n nwork]
 */
#include <u.h>
#include <libc.h>
#include <thread.h>

enum {
	Stack	= 32*1024,
	Maxmsg	= 128*1024,	/* devjs.c:n viesti */
	Maxio	= 64*1024,	/* datan enimmäismäärä viestissä */
	Nfid	= 512,
	Maxwork	= 16,
};

typedef struct Req Req;
struct Req
{
	int	n;
	char	b[1];
};

static int jsfd;
static Channel *reqc;
static QLock fidlk;
static int fids[Nfid];

static void
usage(void)
{
	fprint(2, "usage: aux/jsio [-f file] [-n nwork]\n");
	threadexitsall("usage");
}

/* vastaus: otsikkorivi ja data samana kirjoituksena */
static void
answer(char *tag, char *line, void *data, long n)
{
	char *b;
	int h;

	h = strlen(tag) + 1 + strlen(line) + 1;
	b = malloc(h + n + 1);	/* sprint:n NUL */
	if(b == nil)
		return;
	sprint(b, "%s %s\n", tag, line);
	if(n > 0)
		memmove(b+h, data, n);
	if(write(jsfd, b, h+n) != h+n)
		fprint(2, "jsio: write: %r\n");
	free(b);
}

static void
fail(char *tag)
{
	char e[ERRMAX], *s;

	rerrstr(e, sizeof e);
	s = smprint("error %q", e[0] ? e : "failed");
	answer(tag, s, nil, 0);
	free(s);
}

static int
newfid(int fd)
{
	int i;

	qlock(&fidlk);
	for(i = 1; i < Nfid; i++)
		if(fids[i] == 0){
			fids[i] = fd+1;
			qunlock(&fidlk);
			return i;
		}
	qunlock(&fidlk);
	close(fd);
	werrstr("too many open files");
	return -1;
}

static int
getfd(char *s)
{
	int i, fd;

	i = atoi(s);
	if(i <= 0 || i >= Nfid){
		werrstr("bad fid");
		return -1;
	}
	qlock(&fidlk);
	fd = fids[i] - 1;
	qunlock(&fidlk);
	if(fd < 0)
		werrstr("bad fid");
	return fd;
}

static int
dropfid(char *s)
{
	int i, fd;

	i = atoi(s);
	if(i <= 0 || i >= Nfid){
		werrstr("bad fid");
		return -1;
	}
	qlock(&fidlk);
	fd = fids[i] - 1;
	fids[i] = 0;
	qunlock(&fidlk);
	if(fd < 0){
		werrstr("bad fid");
		return -1;
	}
	return close(fd);
}

static char*
dirline(Dir *d)
{
	return smprint("%q %llux %lud %d %lud %lld %ld %ld %q %q %q\n",
		d->name, d->qid.path, d->qid.vers, d->qid.type, d->mode, d->length,
		d->mtime, d->atime, d->uid, d->gid, d->muid);
}

static void
statreply(char *tag, Dir *d)
{
	char *s;

	if(d == nil){
		fail(tag);
		return;
	}
	s = dirline(d);
	answer(tag, "ok", s, strlen(s));
	free(s);
	free(d);
}

/* K=V-sanat Dir:iin; muut kentät ennallaan (nulldir) */
static int
wstatargs(Dir *d, char **f, int nf)
{
	int i;
	char *v;

	nulldir(d);
	for(i = 0; i < nf; i++){
		if((v = strchr(f[i], '=')) == nil){
			werrstr("bad wstat field");
			return -1;
		}
		*v++ = 0;
		if(strcmp(f[i], "name") == 0)
			d->name = v;
		else if(strcmp(f[i], "gid") == 0)
			d->gid = v;
		else if(strcmp(f[i], "mode") == 0)
			d->mode = strtoul(v, nil, 0);
		else if(strcmp(f[i], "length") == 0)
			d->length = strtoll(v, nil, 0);
		else if(strcmp(f[i], "mtime") == 0)
			d->mtime = strtoul(v, nil, 0);
		else{
			werrstr("bad wstat field %s", f[i]);
			return -1;
		}
	}
	return 0;
}

static void
dirreadreply(char *tag, int fd)
{
	Dir *d;
	char *b, *s, hdr[32];
	long n, len, i, nent;

	b = malloc(Maxio + 512);
	if(b == nil){
		fail(tag);
		return;
	}
	/* dirread palauttaa kokonaisia merkintöjä; enintään Maxio tavua rivejä */
	len = 0;
	nent = 0;
	while(len < Maxio - 8192 && (n = dirread(fd, &d)) > 0){
		for(i = 0; i < n; i++){
			s = dirline(&d[i]);
			memmove(b+len, s, strlen(s));
			len += strlen(s);
			free(s);
			nent++;
		}
		free(d);
	}
	if(n < 0 && nent == 0){
		free(b);
		fail(tag);
		return;
	}
	snprint(hdr, sizeof hdr, "ok %ld", nent);
	answer(tag, hdr, b, len);
	free(b);
}

static void
request(Req *r)
{
	char *nl, *f[16], *data, line[64], *buf;
	int nf, fd, i;
	long n, ndata;
	vlong off;
	Dir d, *dp;

	nl = memchr(r->b, '\n', r->n);
	if(nl == nil)
		return;
	*nl = 0;
	data = nl+1;
	ndata = r->n - (data - r->b);
	nf = tokenize(r->b, f, nelem(f));
	if(nf < 2)
		return;
	if(strcmp(f[1], "ping") == 0){
		answer(f[0], "ok", nil, 0);
	}else if(strcmp(f[1], "stat") == 0 && nf == 3){
		statreply(f[0], dirstat(f[2]));
	}else if(strcmp(f[1], "fstat") == 0 && nf == 3){
		if((fd = getfd(f[2])) < 0)
			fail(f[0]);
		else
			statreply(f[0], dirfstat(fd));
	}else if(strcmp(f[1], "open") == 0 && nf == 4){
		if((fd = open(f[2], atoi(f[3]))) < 0 || (i = newfid(fd)) < 0)
			fail(f[0]);
		else{
			snprint(line, sizeof line, "ok %d %d", i, iounit(fd));
			answer(f[0], line, nil, 0);
		}
	}else if(strcmp(f[1], "create") == 0 && nf == 5){
		if((fd = create(f[2], atoi(f[3]), strtoul(f[4], nil, 0))) < 0 || (i = newfid(fd)) < 0)
			fail(f[0]);
		else{
			snprint(line, sizeof line, "ok %d", i);
			answer(f[0], line, nil, 0);
		}
	}else if(strcmp(f[1], "read") == 0 && nf == 5){
		n = atol(f[4]);
		if(n < 0 || n > Maxio)
			n = Maxio;
		if((fd = getfd(f[2])) < 0 || (buf = malloc(n+1)) == nil){
			fail(f[0]);
			return;
		}
		if((n = pread(fd, buf, n, strtoll(f[3], nil, 0))) < 0)
			fail(f[0]);
		else{
			snprint(line, sizeof line, "ok %ld", n);
			answer(f[0], line, buf, n);
		}
		free(buf);
	}else if(strcmp(f[1], "write") == 0 && nf == 4){
		if(ndata > Maxio){
			werrstr("write too large");
			fail(f[0]);
		}else if((fd = getfd(f[2])) < 0 || (n = pwrite(fd, data, ndata, strtoll(f[3], nil, 0))) < 0)
			fail(f[0]);
		else{
			snprint(line, sizeof line, "ok %ld", n);
			answer(f[0], line, nil, 0);
		}
	}else if(strcmp(f[1], "dirread") == 0 && nf == 3){
		if((fd = getfd(f[2])) < 0)
			fail(f[0]);
		else
			dirreadreply(f[0], fd);
	}else if(strcmp(f[1], "close") == 0 && nf == 3){
		if(dropfid(f[2]) < 0)
			fail(f[0]);
		else
			answer(f[0], "ok", nil, 0);
	}else if(strcmp(f[1], "get") == 0 && nf == 5){
		n = atol(f[4]);
		off = strtoll(f[3], nil, 0);
		if(n < 0 || n > Maxio)
			n = Maxio;
		if((fd = open(f[2], OREAD)) < 0){
			fail(f[0]);
			return;
		}
		dp = dirfstat(fd);
		buf = malloc(n+1);
		if(dp == nil || buf == nil || (n = pread(fd, buf, n, off)) < 0)
			fail(f[0]);
		else{
			snprint(line, sizeof line, "ok %ld %lld", n, dp->length);
			answer(f[0], line, buf, n);
		}
		close(fd);
		free(dp);
		free(buf);
	}else if(strcmp(f[1], "remove") == 0 && nf == 3){
		if(remove(f[2]) < 0)
			fail(f[0]);
		else
			answer(f[0], "ok", nil, 0);
	}else if(strcmp(f[1], "wstat") == 0 && nf >= 3){
		if(wstatargs(&d, f+3, nf-3) < 0 || dirwstat(f[2], &d) < 0)
			fail(f[0]);
		else
			answer(f[0], "ok", nil, 0);
	}else if(strcmp(f[1], "fwstat") == 0 && nf >= 3){
		if((fd = getfd(f[2])) < 0 || wstatargs(&d, f+3, nf-3) < 0 || dirfwstat(fd, &d) < 0)
			fail(f[0]);
		else
			answer(f[0], "ok", nil, 0);
	}else if(strcmp(f[1], "bind") == 0 && nf == 5){
		if(bind(f[2], f[3], atoi(f[4])) < 0)
			fail(f[0]);
		else
			answer(f[0], "ok", nil, 0);
	}else if(strcmp(f[1], "mount") == 0 && (nf == 5 || nf == 6)){
		if((fd = dial(netmkaddr(f[2], "tcp", "9fs"), nil, nil, nil)) < 0)
			fail(f[0]);
		else if(mount(fd, -1, f[3], atoi(f[4]), nf == 6 ? f[5] : "") < 0){
			close(fd);
			fail(f[0]);
		}else{
			close(fd);
			answer(f[0], "ok", nil, 0);
		}
	}else if(strcmp(f[1], "unmount") == 0 && nf == 4){
		if(unmount(strcmp(f[2], "-") == 0 ? nil : f[2], f[3]) < 0)
			fail(f[0]);
		else
			answer(f[0], "ok", nil, 0);
	}else{
		werrstr("bad request %s", f[1]);
		fail(f[0]);
	}
}

static void
workproc(void*)
{
	Req *r;

	threadsetname("jsio work");
	while((r = recvp(reqc)) != nil){
		request(r);
		free(r);
	}
}

void
threadmain(int argc, char **argv)
{
	char *file;
	int i, n, nwork;
	Req *r;

	file = "#j/js";
	nwork = 8;
	ARGBEGIN{
	case 'f':
		file = EARGF(usage());
		break;
	case 'n':
		nwork = atoi(EARGF(usage()));
		break;
	default:
		usage();
	}ARGEND
	if(argc != 0 || nwork < 1 || nwork > Maxwork)
		usage();
	quotefmtinstall();
	if((jsfd = open(file, ORDWR)) < 0)
		sysfatal("%s: %r", file);
	reqc = chancreate(sizeof(Req*), 64);
	/* kaikki prosessit jakavat tiedostokuvaajat ja nimiavaruuden (proccreate: RFMEM) */
	for(i = 0; i < nwork; i++)
		proccreate(workproc, nil, Stack);
	for(;;){
		r = malloc(sizeof *r + Maxmsg);
		if(r == nil)
			sysfatal("malloc: %r");
		if((n = read(jsfd, r->b, Maxmsg)) <= 0)
			sysfatal("%s: %r", file);
		r->n = n;
		r->b[n] = 0;
		sendp(reqc, r);
	}
}
