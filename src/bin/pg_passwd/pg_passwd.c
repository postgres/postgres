/*
 * @(#) pg_passwd.c 1.8 09:13:16 97/07/02		Y. Ichikawa
 */
#include "postgres.h"
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <ctype.h>
#define issaltchar(c)	(isalnum(c) || (c) == '.' || (c) == '/')

#ifdef HAVE_TERMIOS_H
#include <termios.h>
#endif
#ifdef HAVE_CRYPT_H
#include <crypt.h>
#else
extern char *crypt(const char *, const char *);

#endif

char	   *comname;
static void usage(FILE *stream);
static void read_pwd_file(char *filename);
static void write_pwd_file(char *filename, char *bkname);
static void encrypt_pwd(char key[9], char salt[3], char passwd[14]);
static void prompt_for_username(char *username);
static void prompt_for_password(char *prompt, char *password);

static void
usage(FILE *stream)
{
	fprintf(stream, "Usage: %s <password file>\n", comname);
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
#ifndef __CYGWIN32__
	fp = fopen(filename, "r");
#else
	fp = fopen(filename, "rb");
#endif
	if (fp == NULL)
	{
		if (errno == ENOENT)
		{
			printf("File \"%s\" does not exist.  Create? (y/n): ", filename);
			fflush(stdout);
			fgets(ans, 128, stdin);
			switch (ans[0])
			{
				case 'y':
				case 'Y':
#ifndef __CYGWIN32__
					fp = fopen(filename, "w");
#else
					fp = fopen(filename, "wb");
#endif
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
	for (npwds = 0; npwds < MAXPWDS && fgets(line, 512, fp) != NULL; ++npwds)
	{
		int			l;
		char	   *p,
				   *q;

		l = strlen(line);
		if (line[l - 1] == '\n')
			line[l - 1] = '\0';
		else
		{						/* too long */
			fprintf(stderr, "%s: line %d: line too long.\n",
					filename, npwds + 1);
			exit(1);
		}

		/* get user name */
		p = line;
		if ((q = strchr(p, ':')) == NULL)
		{
			fprintf(stderr, "%s: line %d: illegal format.\n",
					filename, npwds + 1);
			exit(1);
		}
		*(q++) = '\0';
		if (strlen(p) == 0)
		{
			fprintf(stderr, "%s: line %d: null user name.\n",
					filename, npwds + 1);
			exit(1);
		}
		pwds[npwds].uname = strdup(p);

		/* check duplicate */
		for (i = 0; i < npwds; ++i)
		{
			if (strcmp(pwds[i].uname, pwds[npwds].uname) == 0)
			{
				fprintf(stderr, "%s: duplicated entry.\n", pwds[npwds].uname);
				exit(1);
			}
		}

		/* get password field */
		p = q;
		q = strchr(p, ':');

		/*
		 * --- don't care ----- if ((q = strchr(p, ':')) == NULL) {
		 * fprintf(stderr, "%s: line %d: illegal format.\n", filename,
		 * npwds + 1); exit(1); }
		 */

		if (q != NULL)
			*(q++) = '\0';
		if (strlen(p) != 13)
		{
			fprintf(stderr, "WARNING: %s: line %d: illegal password length.\n",
					filename, npwds + 1);
		}
		pwds[npwds].pwd = strdup(p);

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
#ifndef __CYGWIN32__
	if ((fp = fopen(filename, "w")) == NULL)
#else
	if ((fp = fopen(filename, "wb")) == NULL)
#endif
	{
		perror(filename);
		exit(1);
	}

	/* write file */
	for (i = 0; i < npwds; ++i)
	{
		fprintf(fp, "%s:%s%s%s\n", pwds[i].uname, pwds[i].pwd,
				pwds[i].rest ? ":" : "",
				pwds[i].rest ? pwds[i].rest : "");
	}

	fclose(fp);
}

static void
encrypt_pwd(char key[9], char salt[3], char passwd[14])
{
	int			n;

	/* get encrypted password */
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
	strcpy(passwd, crypt(key, salt));

	/* show it */

	/*
	 * fprintf(stderr, "key = %s, salt = %s, password = %s\n", key, salt,
	 * passwd);
	 */
}

#ifdef NOT_USED
static int
check_pwd(char key[9], char passwd[14])
{
	char		shouldbe[14];
	char		salt[3];

	salt[0] = passwd[0];
	salt[1] = passwd[1];
	salt[2] = '\0';
	encrypt_pwd(key, salt, shouldbe);

	return strncmp(shouldbe, passwd, 13) == 0 ? 1 : 0;
}

#endif

static void
prompt_for_username(char *username)
{
	int			length;

	printf("Username: ");
	fgets(username, 9, stdin);
	length = strlen(username);

	/* skip rest of the line */
	if (length > 0 && username[length - 1] != '\n')
	{
		static char buf[512];

		do
		{
			fgets(buf, 512, stdin);
		} while (buf[strlen(buf) - 1] != '\n');
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

	printf(prompt);
#ifdef HAVE_TERMIOS_H
	tcgetattr(0, &t);
	t_orig = t;
	t.c_lflag &= ~ECHO;
	tcsetattr(0, TCSADRAIN, &t);
#endif
	fgets(password, 9, stdin);
#ifdef HAVE_TERMIOS_H
	tcsetattr(0, TCSADRAIN, &t_orig);
#endif

	length = strlen(password);
	/* skip rest of the line */
	if (length > 0 && password[length - 1] != '\n')
	{
		static char buf[512];

		do
		{
			fgets(buf, 512, stdin);
		} while (buf[strlen(buf) - 1] != '\n');
	}
	if (length > 0 && password[length - 1] == '\n')
		password[length - 1] = '\0';
	printf("\n");
}


int
main(int argc, char *argv[])
{
	static char bkname[512];
	char		username[9];
	char		salt[3];
	char		key[9],
				key2[9];
	char		e_passwd[14];
	int			i;

	comname = argv[0];
	if (argc != 2)
	{
		usage(stderr);
		exit(1);
	}


	/* open file */
	read_pwd_file(argv[1]);

	/* ask for the user name and the password */
	prompt_for_username(username);
	prompt_for_password("New password: ", key);
	prompt_for_password("Re-enter new password: ", key2);
	if (strncmp(key, key2, 8) != 0)
	{
		fprintf(stderr, "Password mismatch.\n");
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
			fprintf(stderr, "%s: cannot handle so may entries.\n", comname);
			exit(1);
		}
		pwds[npwds].uname = strdup(username);
		pwds[npwds].pwd = strdup(e_passwd);
		pwds[npwds].rest = NULL;
		++npwds;
	}

	/* write back the file */
	sprintf(bkname, "%s.bk", argv[1]);
	write_pwd_file(argv[1], bkname);

	return 0;
}
