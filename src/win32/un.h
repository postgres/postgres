#ifndef _SYS_UN_H
#define _SYS_UN_H

/* JKR added file, all hacks will be in the files added, not in EGCS */

struct sockaddr_un
{
	short		sun_family;		/* AF_UNIX */
	char		sun_path[108];	/* path name (gag) */
};

#endif /* _SYS_UN_H */
