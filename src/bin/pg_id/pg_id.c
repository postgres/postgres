/*
 * pg_id.c
 *
 * A crippled id utility for use in various shell scripts in use by PostgreSQL
 * (in particular initdb)
 *
 * Copyright (c) 2000-2003, PostgreSQL Global Development Group
 *
 * $Header: /cvsroot/pgsql/src/bin/pg_id/Attic/pg_id.c,v 1.23 2003/09/06 01:41:56 momjian Exp $
 */
#include "postgres_fe.h"

#ifdef HAVE_GETOPT_H
#include <getopt.h>
#endif

#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>

int
main(int argc, char *argv[])
{
	int			c;
	int			name_only_flag = 0,
				use_real_uid_flag = 0,
				limit_user_info = 0;
	const char *username = NULL;

	struct passwd *pw;

	extern int	optind;

	while ((c = getopt(argc, argv, "nru")) != -1)
	{
		switch (c)
		{
			case 'n':
				name_only_flag = 1;
				break;
			case 'r':
				use_real_uid_flag = 1;
				break;
			case 'u':
				limit_user_info = 1;
				break;
			default:
				fprintf(stderr, "Usage: %s [-n] [-r] [-u] [username]\n", argv[0]);
				exit(1);
		}
	}

	if (argc - optind >= 1)
		username = argv[optind];

	if (name_only_flag && !limit_user_info)
	{
		fprintf(stderr, "%s: -n must be used together with -u\n", argv[0]);
		exit(1);
	}
	if (username && use_real_uid_flag)
	{
		fprintf(stderr, "%s: -r cannot be used when a user name is given\n", argv[0]);
		exit(1);
	}


	if (username)
	{
		pw = getpwnam(username);
		if (!pw)
		{
			fprintf(stderr, "%s: %s: no such user\n", argv[0], username);
			exit(1);
		}
	}
	else if (use_real_uid_flag)
		pw = getpwuid(getuid());
	else
		pw = getpwuid(geteuid());

	if (!pw)
	{
		perror(argv[0]);
		exit(1);
	}

	if (!limit_user_info)
		printf("uid=%d(%s)\n", (int) pw->pw_uid, pw->pw_name);
	else if (name_only_flag)
		puts(pw->pw_name);
	else
#ifdef __BEOS__
	if (pw->pw_uid == 0)
		printf("1\n");
	else
#endif
		printf("%d\n", (int) pw->pw_uid);

	return 0;
}
