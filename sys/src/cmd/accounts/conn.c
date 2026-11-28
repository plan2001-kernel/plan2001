/*
 * A connection to auth/accountd as its own programs make one (as
 * aux/etherws does for a page): a pipe posted in /srv (SRV.PID.RANDOM,
 * mode 0600, the poster's: accountd takes an admin connection only from
 * the host owner's; the random part keeps others from guessing the name
 * and posting their own) and "conn SRV.PID.RANDOM admin -" written to
 * /srv/SRV.  A message is a write: T and its text; X alone ends the
 * connection.
 */
#include <u.h>
#include <libc.h>
#include "conn.h"

/*
 * Each connection's /srv file, by the connection's fd: aux/ethernetd dials
 * from several procs at once, so one shared fd would let one hangup close
 * (and remove) another connection's /srv file before accountd opened it.
 */
enum {
	Nconn	= 32,
};
static struct {
	Lock;
	int	fd[Nconn];
	int	srvfd[Nconn];
	int	n;
} conns;

static int
addconn(int fd, int srvfd)
{
	int i;

	lock(&conns);
	for(i = 0; i < conns.n; i++)
		if(conns.fd[i] < 0)
			break;
	if(i == Nconn){
		unlock(&conns);
		werrstr("too many accountd connections");
		return -1;
	}
	if(i == conns.n)
		conns.n++;
	conns.fd[i] = fd;
	conns.srvfd[i] = srvfd;
	unlock(&conns);
	return 0;
}

static int
delconn(int fd)
{
	int i, srvfd;

	srvfd = -1;
	lock(&conns);
	for(i = 0; i < conns.n; i++)
		if(conns.fd[i] == fd){
			srvfd = conns.srvfd[i];
			conns.fd[i] = -1;
			break;
		}
	unlock(&conns);
	return srvfd;
}

int
dialaccountd(char *srv)
{
	char name[64], path[80];
	int p[2], ctl, srvfd;

	if(pipe(p) < 0)
		return -1;
	snprint(name, sizeof name, "%s.%d.%lud", srv, getpid(), truerand());
	snprint(path, sizeof path, "/srv/%s", name);
	if((srvfd = create(path, OWRITE|ORCLOSE, 0600)) < 0)
		goto Err;
	if(fprint(srvfd, "%d", p[0]) < 0)
		goto Err;
	close(p[0]);
	p[0] = -1;
	snprint(path, sizeof path, "/srv/%s", srv);
	if((ctl = open(path, OWRITE)) < 0){
		werrstr("%s: %r", path);
		goto Err;
	}
	if(fprint(ctl, "conn %s admin -", name) < 0){
		close(ctl);
		goto Err;
	}
	close(ctl);
	if(addconn(p[1], srvfd) < 0)
		goto Err;
	return p[1];
Err:
	if(p[0] >= 0)
		close(p[0]);
	close(p[1]);
	if(srvfd >= 0)
		close(srvfd);
	return -1;
}

void
hangupaccountd(int fd)
{
	int srvfd;

	write(fd, "X", 1);
	close(fd);
	if((srvfd = delconn(fd)) >= 0)
		close(srvfd);
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
