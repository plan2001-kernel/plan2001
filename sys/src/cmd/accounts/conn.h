enum {
	Maxmsg	= 16*1024,	/* a message's text, at most */
};

int	dialaccountd(char *srv);
void	hangupaccountd(int fd);
char*	askaccountd(int fd, char *req);
