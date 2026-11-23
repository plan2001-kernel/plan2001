/*
 * A long print grows the format buffer many times.  This checks the
 * realloc'd buffer's cursor and contents (libc/vsmprint.c and
 * libc/runevsmprint.c), not the formatter's spelling.
 */
#include <u.h>
#include <libc.h>

enum { N = 200000 };

/* fmt.o refers to these kernel/console symbols; the test needs neither. */
int
__errfmt(void *f)
{
	(void)f;
	return 0;
}

int
main(void)
{
	char *s, *p;
	Rune *r, *rs;
	int i;

	p = malloc(N+1);
	if(p == nil)
		return 1;
	for(i = 0; i < N; i++)
		p[i] = 'a';
	p[N] = 0;
	s = smprint("%s", p);
	if(s == nil)
		return 2;
	if(strlen(s) != N)
		return 3;
	if(s[0] != 'a' || s[N/2] != 'a' || s[N-1] != 'a' || s[N] != 0)
		return 4;
	free(s);
	free(p);

	r = malloc(sizeof(Rune)*(N+1));
	if(r == nil)
		return 5;
	for(i = 0; i < N; i++)
		r[i] = 'a';
	r[N] = 0;
	rs = runesmprint("%S", r);
	if(rs == nil)
		return 6;
	if(runestrlen(rs) != N)
		return 7;
	if(rs[0] != 'a' || rs[N/2] != 'a' || rs[N-1] != 'a' || rs[N] != 0)
		return 8;
	free(rs);
	free(r);

	return 0;
}
