/*
 * @(#) pg_passwd.c 1.8 09:13:16 97/07/02		Y. Ichikawa
 */
#include "postgres_fe.h"

#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <ctype.h>
#define issaltchar(c)	(isalnum((unsigned char) (c)) || (c) == '.' || (c) == '/')

#ifdef HAVE_TERMIOS_H
#include <termios.h>
#endif
#ifdef HAVE_CRYPT_H
#include <crypt.h>
#else
extern char *crypt(const char *, const char *);

#endif

/*
 * We assume that the output of crypt(3) is always 13 characters,
 * and that at most 8 characters can usefully be sent to it.
 *
 * Postgres usernames are assumed to be less than NAMEDATALEN chars long.
 */
#define CLEAR_PASSWD_LEN 8			/* not including null */
#define CRYPTED_PASSWD_LEN 13		/* not including null */

const char * progname;

static void usage(void);
static void read_pwd_file(char *filename);
static void write_pwd_file(char *filename, char *bkname);
static void encrypt_pwd(char key[CLEAR_PASSWD_LEN+1],
						char salt[3],
						char passwd[CRYPTED_PASSWD_LEN+1]);
static void prompt_for_username(char *username);
static void prompt_for_password(char *prompt, char *password);

static void
usage(void)
{
	printf("%s manipulates flat text password files for PostgreSQL.\n\n", progname);
	printf("Usage:\n  %s PASSWORD-FILE\n\n", progname);
	printf("Report bugs to <pgsql-bugs@postgresql.org>.\n");
}

typedef struct
{
	char	   *uname;
	char	   *pwd;
	char	   *rest;
} pg_pwd;

#define MAXPWDS 1024

pg_pwd		pwds[MAXPWDS];
int			npwds = 0;


static void
read_pwd_file(char *filename)
{
	FILE	   *fp;
	static char line[512];
	static char ans[128];
	int			i;

try_again:
	fp = fopen(filename, PG_BINARY_R);
	if (fp == NULL)
	{
		if (errno == ENOENT)
		{
			printf("File \"%s\" does not exist.  Create? (y/n): ", filename);
			fflush(stdout);
			if (fgets(ans, sizeof(ans), stdin) == NULL)
				exit(1);
			switch (ans[0])
			{
				case 'y':
				case 'Y':
					fp = fopen(filename, PG_BINARY_W);
					if (fp == NULL)
					{
						perror(filename);
						exit(1);
					}
					fclose(fp);
					goto try_again;
				default:
					/* cannot continue */
					exit(1);
			}
		}
		else
		{
			perror(filename);
			exit(1);
		}
	}

	/* read all the entries */
	for (npwds = 0;
		 npwds < MAXPWDS && fgets(line, sizeof(line), fp) != NULL;
		 ++npwds)
	{
		int			l;
		char	   *p,
				   *q;

		l = strlen(line);
		if (line[l - 1] == '\n')
			line[l - 1] = '\0';
		else
		{
			fprintf(stderr, "%s:%d: line too long\n",
					filename, npwds + 1);
			exit(1);
		}

		/* get user name */
		p = line;
		if ((q = strchr(p, ':')) != NULL)
			*q = '\0';

		if (strlen(p) == 0)
		{
			fprintf(stderr, "%s:%d: null user name\n",
					filename, npwds + 1);
			exit(1);
		}
		pwds[npwds].uname = strdup(p);

		/* check for duplicate user name */
		for (i = 0; i < npwds; ++i)
		{
			if (strcmp(pwds[i].uname, pwds[npwds].uname) == 0)
			{
				fprintf(stderr, "Duplicate username %s in entry %d\n",
						pwds[npwds].uname, npwds+1);
				exit(1);
			}
		}

		/* get password field */
		if (q)
		{
			p = q + 1;
			q = strchr(p, ':');

			if (q != NULL)
				*(q++) = '\0';

			if (strlen(p) != CRYPTED_PASSWD_LEN && strcmp(p, "+") != 0)
			{
				fprintf(stderr, "%s:%d: warning: invalid password length\n",
						filename, npwds + 1);
			}
			pwds[npwds].pwd = strdup(p);
		}
		else
			pwds[npwds].pwd = NULL;

		/* rest of the line is treated as is */
		if (q == NULL)
			pwds[npwds].rest = NULL;
		else
			pwds[npwds].rest = strdup(q);
	}

	fclose(fp);
}

static void
write_pwd_file(char *filename, char *bkname)
{
	FILE	   *fp;
	int			i;

	/* make the backup file */
link_again:
	if (link(filename, bkname))
	{
		if (errno == EEXIST)
		{
			unlink(bkname);
			goto link_again;
		}
		perror(bkname);
		exit(1);
	}
	if (unlink(filename))
	{
		perror(filename);
		exit(1);
	}

	/* open file */
	if ((fp = fopen(filename, PG_BINARY_W)) == NULL)
	{
		perror(filename);
		exit(1);
	}

	/* write file */
	for (i = 0; i < npwds; ++i)
	{
		fprintf(fp, "%s", pwds[i].uname);
		if (pwds[i].pwd)
			fprintf(fp, ":%s", pwds[i].pwd);
		if (pwds[i].rest)
			fprintf(fp, ":%s", pwds[i].rest);
		fprintf(fp, "\n");
	}

	fclose(fp);
}

static void
encrypt_pwd(char key[CLEAR_PASSWD_LEN+1],
			char salt[3],
			char passwd[CRYPTED_PASSWD_LEN+1])
{
	int			n;

	/* select a salt, if not already given */
	if (salt[0] == '\0')
	{
		srand(time(NULL));
		do
		{
			n = rand() % 256;
		} while (!issaltchar(n));
		salt[0] = n;
		do
		{
			n = rand() % 256;
		} while (!issaltchar(n));
		salt[1] = n;
		salt[2] = '\0';
	}

	/* get encrypted password */
	strcpy(passwd, crypt(key, salt));

#ifdef PG_PASSWD_DEBUG
	/* show it */
	fprintf(stderr, "key = %s, salt = %s, password = %s\n",
			key, salt, passwd);
#endif
}

static void
prompt_for_username(char *username)
{
	int			length;

	printf("Username: ");
	fflush(stdout);
	if (fgets(username, NAMEDATALEN, stdin) == NULL)
		username[0] = '\0';

	length = strlen(username);
	if (length > 0 && username[length - 1] != '\n')
	{
		/* eat rest of the line */
		char		buf[128];
		int			buflen;

		do
		{
			if (fgets(buf, sizeof(buf), stdin) == NULL)
				break;
			buflen = strlen(buf);
		} while (buflen > 0 && buf[buflen - 1] != '\n');
	}
	if (length > 0 && username[length - 1] == '\n')
		username[length - 1] = '\0';
}

static void
prompt_for_password(char *prompt, char *password)
{
	int			length;

#ifdef HAVE_TERMIOS_H
	struct termios t_orig,
				t;

#endif

#ifdef HAVE_TERMIOS_H
	tcgetattr(0, &t);
	t_orig = t;
	t.c_lflag &= ~ECHO;
	tcsetattr(0, TCSADRAIN, &t);
#endif

	printf(prompt);
	fflush(stdout);

	if (fgets(password, CLEAR_PASSWD_LEN+1, stdin) == NULL)
		password[0] = '\0';

#ifdef HAVE_TERMIOS_H
	tcsetattr(0, TCSADRAIN, &t_orig);
#endif

	length = strlen(password);
	if (length > 0 && password[length - 1] != '\n')
	{
		/* eat rest of the line */
		char		buf[128];
		int			buflen;

		do
		{
			if (fgets(buf, sizeof(buf), stdin) == NULL)
				break;
			buflen = strlen(buf);
		} while (buflen > 0 && buf[buflen - 1] != '\n');
	}
	if (length > 0 && password[length - 1] == '\n')
		password[length - 1] = '\0';
	printf("\n");
}


int
main(int argc, char *argv[])
{
	char       *filename;
	char		bkname[MAXPGPATH];
	char		username[NAMEDATALEN];
	char		salt[3];
	char		key[CLEAR_PASSWD_LEN + 1],
				key2[CLEAR_PASSWD_LEN + 1];
	char		e_passwd[CRYPTED_PASSWD_LEN + 1];
	int			i;

	progname = argv[0];

	if (argc != 2)
	{
		fprintf(stderr, "%s: too %s arguments\nTry '%s --help' for more information.\n",
				progname, argc > 2 ? "many" : "few", progname);
		exit(1);
	}

	if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-?") == 0)
	{
		usage();
		exit(0);
	}
	if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0)
	{
		puts("pg_passwd (PostgreSQL) " PG_VERSION);
		exit(0);
	}
	if (argv[1][0] == '-')
	{
		fprintf(stderr, "%s: invalid option: %s\nTry '%s --help' for more information.\n",
				progname, argv[1], progname);
		exit(1);
	}

	filename = argv[1];

	/* open file */
	read_pwd_file(filename);

	/* ask for the user name and the password */
	prompt_for_username(username);
	prompt_for_password("New password: ", key);
	prompt_for_password("Re-enter new password: ", key2);
	if (strcmp(key, key2) != 0)
	{
		fprintf(stderr, "Password mismatch\n");
		exit(1);
	}
	salt[0] = '\0';
	encrypt_pwd(key, salt, e_passwd);

	/* check password entry */
	for (i = 0; i < npwds; ++i)
	{
		if (strcmp(pwds[i].uname, username) == 0)
		{						/* found */
			pwds[i].pwd = strdup(e_passwd);
			break;
		}
	}
	if (i == npwds)
	{							/* did not exist */
		if (npwds == MAXPWDS)
		{
			fprintf(stderr, "Cannot handle so many entries\n");
			exit(1);
		}
		pwds[npwds].uname = strdup(username);
		pwds[npwds].pwd = strdup(e_passwd);
		pwds[npwds].rest = NULL;
		++npwds;
	}

	/* write back the file */
	sprintf(bkname, "%s.bk", filename);
	write_pwd_file(filename, bkname);

	return 0;
}
