/*
 * pg_id.c
 *
 * A crippled id utility for use in various shell scripts in use by PostgreSQL
 * (in particular initdb)
 *
 * Copyright (c) 2000-2003, PostgreSQL Global Development Group
 *
 * $Header: /cvsroot/pgsql/src/bin/pg_id/Attic/pg_id.c,v 1.25 2003/10/10 01:34:51 momjian Exp $
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
	extern int	optind;
#ifndef WIN32
	struct passwd *pw;
#else
	struct passwd_win32
	{
		int pw_uid;
		char pw_name[128];
	} pass_win32;
	struct passwd_win32 *pw = &pass_win32;
	unsigned long pwname_size = sizeof(pass_win32.pw_name) - 1;

	pw->pw_uid = 1;
#endif

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
#ifndef WIN32
				fprintf(stderr, "Usage: %s [-n] [-r] [-u] [username]\n", argv[0]);
#else
				fprintf(stderr, "Usage: %s [-n] [-r] [-u]\n", argv[0]);
#endif
				exit(1);
		}
	}

	if (argc - optind >= 1)
#ifndef WIN32
		username = argv[optind];
#else
	{
		fprintf(stderr, "%s: specifying a username is not supported on this platform\n", argv[0]);
		exit(1);
	}	
#endif

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

#ifndef WIN32
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
#else
	if (!use_real_uid_flag)
	{
		fprintf(stderr, "%s: -r must be used on this platform\n", argv[0]);
		exit(1);
	}

	GetUserName(pw->pw_name, &pwname_size);
#endif

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
