/*
 * A connection to auth/accountd as its own programs make one (as
 * aux/etherws does for a page): a pipe posted in /srv (SRV.PID, mode
 * 0600, the poster's: accountd takes an admin connection only from the
 * host owner's) and "conn SRV.PID admin -" written to /srv/SRV.  A
 * message is a write: T and its text; X alone ends the connection.
 */
#include <u.h>
#include <libc.h>
#include "conn.h"

static int srvfd = -1;

int
dialaccountd(char *srv)
{
	char name[64], path[80];
	int p[2], ctl;

	if(pipe(p) < 0)
		return -1;
	snprint(name, sizeof name, "%s.%d", srv, getpid());
	snprint(path, sizeof path, "/srv/%s", name);
	if((srvfd = create(path, OWRITE|ORCLOSE, 0600)) < 0)
		return -1;
	if(fprint(srvfd, "%d", p[0]) < 0)
		return -1;
	close(p[0]);
	snprint(path, sizeof path, "/srv/%s", srv);
	if((ctl = open(path, OWRITE)) < 0){
		werrstr("%s: %r", path);
		return -1;
	}
	if(fprint(ctl, "conn %s admin -", name) < 0)
		return -1;
	close(ctl);
	return p[1];
}

void
hangupaccountd(int fd)
{
	write(fd, "X", 1);
	close(fd);
	if(srvfd >= 0)
		close(srvfd);
	srvfd = -1;
}

/* a request, its answer (malloced, NUL-terminated); nil if the connection ended */
char*
askaccountd(int fd, char *req)
{
	char *m, *a;
	int n;

	m = smprint("T%s", req);
	n = strlen(m);
	if(write(fd, m, n) != n){
		free(m);
		return nil;
	}
	free(m);
	a = malloc(Maxmsg+1);
	if(a == nil)
		return nil;
	/* accountd answers at once, or not at all (a connection it refused): the alarm ends the wait */
	alarm(30*1000);
	n = read(fd, a, Maxmsg);
	alarm(0);
	if(n <= 1 || a[0] != 'T'){
		free(a);
		werrstr("accountd ended the connection");
		return nil;
	}
	a[n] = 0;
	memmove(a, a+1, n);
	return a;
}
