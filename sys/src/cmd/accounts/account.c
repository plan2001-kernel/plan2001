/*
 * account - the host owner's requests to auth/accountd (accountd.c has
 * them): invitations, accounts, their passkeys, groups and sessions.
 * The answer's first line is ok or error WHY; a list follows ok a line
 * an item.  The status is the error.
 *
 *	auth/account [-s srvname] request [arg...]
 *	auth/account invite alice 86400 lab
 *	auth/account users
 */
#include <u.h>
#include <libc.h>
#include "conn.h"

static void
usage(void)
{
	fprint(2, "usage: auth/account [-s srvname] request [arg...]\n");
	exits("usage");
}

void
main(int argc, char **argv)
{
	char *srv, *req, *a, *s;
	int fd, i;

	srv = "accountd";
	ARGBEGIN{
	case 's':
		srv = EARGF(usage());
		break;
	default:
		usage();
	}ARGEND
	if(argc < 1)
		usage();
	req = strdup("");
	for(i = 0; i < argc; i++){
		if(argv[i][0] == 0 || strpbrk(argv[i], " \t\r\n") != nil)
			sysfatal("bad argument: %q", argv[i]);
		s = smprint("%s%s%s", req, i ? " " : "", argv[i]);
		free(req);
		req = s;
	}
	if((fd = dialaccountd(srv)) < 0)
		sysfatal("accountd: %r");
	if((a = askaccountd(fd, req)) == nil)
		sysfatal("accountd: %r");
	hangupaccountd(fd);
	print("%s\n", a);
	if(strncmp(a, "error", 5) == 0)
		exits(a[5] == ' ' ? a+6 : a);
	exits(nil);
}
