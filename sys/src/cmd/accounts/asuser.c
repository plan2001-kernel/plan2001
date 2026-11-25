/*
 * asuser - run a command as the user a passkey session (auth/accountd)
 * names, as a cpu server runs one for a user it authenticated.  The host
 * owner's: a broker for a service the page reaches (README.md, "Accounts
 * and passkeys").  A passkey's assertion is not a 9P attach: the session
 * gets from accountd the user's dp9ik key (keyfs's), and with it a
 * factotum of its own (no /srv, this process's namespace only)
 * authenticates as the user to the host owner's factotum (dp9ik, the auth
 * server's tickets), as a cpu server's client would.  The capability
 * that gives makes this process the user (auth_chuid: /dev/capuse,
 * newns): then the command, in the user's namespace, with the file
 * server checking the user's permissions as for anyone.
 *
 * The session's token is the first line of standard input (never an
 * argument: ps would show it); the rest is the command's.
 *
 * The factotum finds the auth server as any does: ndb's auth= for the
 * domain (authdom=).
 *
 *	auth/asuser [-s srvname] [-n namespace] cmd [arg...]
 */
#include <u.h>
#include <libc.h>
#include <auth.h>
#include "conn.h"

static void
usage(void)
{
	fprint(2, "usage: auth/asuser [-s srvname] [-n namespace] cmd [arg...]\n");
	exits("usage");
}

/* a line of standard input, a byte at a time: the rest is the command's */
static char*
readline(char *buf, int n)
{
	int i;

	for(i = 0; i < n-1; i++){
		if(read(0, buf+i, 1) != 1)
			return nil;
		if(buf[i] == '\n')
			break;
	}
	if(i == n-1)
		return nil;
	buf[i] = 0;
	if(i > 0 && buf[i-1] == '\r')
		buf[i-1] = 0;
	return buf;
}

/* the client's side, with the user's key in a factotum of its own */
static void
client(int fd, char *name, char *dom, char *key)
{
	AuthRpc *rpc;
	AuthInfo *ai;
	int pid, afd, ctl;
	Waitmsg *w;

	rfork(RFNAMEG|RFNOTEG|RFENVG);
	switch(pid = fork()){
	case -1:
		sysfatal("fork: %r");
	case 0:
		/* no -s: posted nowhere; -n: no secstore */
		execl("/bin/auth/factotum", "factotum", "-n", "-m", "/n", nil);
		sysfatal("factotum: %r");
	}
	while((w = wait()) != nil){
		if(w->pid == pid)
			break;
		free(w);
	}
	if(w == nil || w->msg[0] != 0)
		sysfatal("factotum: %s", w ? w->msg : "gone");
	free(w);
	if((ctl = open("/n/factotum/ctl", OWRITE)) < 0)
		sysfatal("factotum: %r");
	/* !password=______: the client's key query asks for !password?, as factotum's own !hex keys have it */
	if(fprint(ctl, "key proto=dp9ik dom=%q user=%q !hex=%s !password=______", dom, name, key) < 0)
		sysfatal("factotum: %r");
	close(ctl);
	memset(key, 0, strlen(key));
	if((afd = open("/n/factotum/rpc", ORDWR)) < 0)
		sysfatal("factotum: %r");
	if((rpc = auth_allocrpc(afd)) == nil)
		sysfatal("factotum: %r");
	ai = fauth_proxy(fd, rpc, nil, smprint("proto=dp9ik role=client dom=%q user=%q", dom, name));
	if(ai == nil)
		sysfatal("as %s: %r", name);
	auth_freeAI(ai);
	auth_freerpc(rpc);
	exits(nil);
}

void
main(int argc, char **argv)
{
	char tok[128], *a, *f[5], *ns, *srv;
	int fd, p[2], pid;
	AuthInfo *ai;
	Waitmsg *w;

	quotefmtinstall();
	srv = "accountd";
	ns = nil;
	ARGBEGIN{
	case 's':
		srv = EARGF(usage());
		break;
	case 'n':
		ns = EARGF(usage());
		break;
	default:
		usage();
	}ARGEND
	if(argc < 1)
		usage();
	if(readline(tok, sizeof tok) == nil)
		sysfatal("no session");

	if((fd = dialaccountd(srv)) < 0)
		sysfatal("accountd: %r");
	a = smprint("asuser %s", tok);
	memset(tok, 0, sizeof tok);
	if((a = askaccountd(fd, a)) == nil)
		sysfatal("accountd: %r");
	hangupaccountd(fd);
	if(tokenize(a, f, nelem(f)) != 4 || strcmp(f[0], "ok") != 0)
		sysfatal("%s", strncmp(a, "error ", 6) == 0 ? a+6 : "accountd: bad answer");

	if(pipe(p) < 0)
		sysfatal("pipe: %r");
	switch(pid = fork()){
	case -1:
		sysfatal("fork: %r");
	case 0:
		close(p[0]);
		client(p[1], f[1], f[2], f[3]);
	}
	memset(f[3], 0, strlen(f[3]));
	close(p[1]);

	/* the server's side: the host owner's factotum, the auth server's ticket */
	ai = auth_proxy(p[0], nil, "proto=dp9ik role=server dom=%q", f[2]);
	close(p[0]);
	while((w = wait()) != nil){
		if(w->pid == pid)
			break;
		free(w);
	}
	if(ai == nil)
		sysfatal("as %s: %r", f[1]);
	if(w == nil || w->msg[0] != 0)
		sysfatal("as %s: %s", f[1], w ? w->msg : "client gone");
	free(w);
	if(strcmp(ai->cuid, f[1]) != 0)
		sysfatal("authenticated as %s, not %s", ai->cuid, f[1]);
	/*
	 * auth_chuid mounts /srv/factotum on /mnt before newns: in a copy of
	 * the namespace, not the caller's (an rcpu session's /mnt/term)
	 */
	rfork(RFNAMEG);
	if(auth_chuid(ai, ns) < 0)
		sysfatal("as %s: %r", f[1]);
	auth_freeAI(ai);
	free(a);
	exec(argv[0], argv);
	if(argv[0][0] != '/' && strncmp(argv[0], "./", 2) != 0)
		exec(smprint("/bin/%s", argv[0]), argv);
	sysfatal("%s: %r", argv[0]);
}
