/*
 * PostgreSQL type definitions for chkpass
 * Written by D'Arcy J.M. Cain
 * darcy@druid.net
 * http://www.druid.net/darcy/
 *
 * $Header: /cvsroot/pgsql/contrib/chkpass/chkpass.c,v 1.1 2001/05/03 12:32:13 darcy Exp $
 * best viewed with tabs set to 4
 */

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <postgres.h>
#include <utils/palloc.h>

/*
 * This type encrypts it's input unless the first character is a colon.
 * The output is the encrypted form with a leading colon.  The output
 * format is designed to allow dump and reload operations to work as
 * expected without doing special tricks.
 */


/*
 * This is the internal storage format for CHKPASSs.
 * 15 is all I need but add a little buffer
 */

typedef struct chkpass
{
	char	password[16];
}			chkpass;

/*
 * Various forward declarations:
 */

chkpass	   *chkpass_in(char *str);
char	   *chkpass_out(chkpass * addr);
text	   *chkpass_rout(chkpass * addr);

/* Only equal or not equal make sense */
bool		chkpass_eq(chkpass * a1, text * a2);
bool		chkpass_ne(chkpass * a1, text * a2);

/* This function checks that the password is a good one
 * It's just a placeholder for now */
static int
verify_pass(const char *str)
{
	return 0;
}

/*
 * CHKPASS reader.
 */
chkpass *
chkpass_in(char *str)
{
	chkpass	   *result;
	char		mysalt[4];
	static bool	random_initialized = false;
	static char salt_chars[] =
		"./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";

	/* special case to let us enter encrypted passwords */
	if (*str == ':')
	{
		result = (chkpass *) palloc(sizeof(chkpass));
		strncpy(result->password, str + 1, 13);
		result->password[13] = 0;
		return (result);
	}

	if (verify_pass(str) != 0)
	{
		elog(ERROR, "chkpass_in: purported CHKPASS \"%s\" is a weak password",
		     str);
		return NULL;
	}

	result = (chkpass *) palloc(sizeof(chkpass));

	if (!random_initialized)
	{
		srandom((unsigned int) time(NULL));
		random_initialized = true;
	}

	mysalt[0] = salt_chars[random() & 0x3f];
	mysalt[1] = salt_chars[random() & 0x3f];
	mysalt[2] = 0;				/* technically the terminator is not
								 * necessary but I like to play safe */
	strcpy(result->password, crypt(str, mysalt));
	return (result);
}

/*
 * CHKPASS output function.
 * Just like any string but we know it is max 15 (13 plus colon and terminator.)
 */

char *
chkpass_out(chkpass * password)
{
	char	   *result;

	if (password == NULL)
		return (NULL);

	if ((result = (char *) palloc(16)) != NULL)
	{
		result[0] = ':';
		strcpy(result + 1, password->password);
	}

	return (result);
}


/*
 * special output function that doesn't output the colon
 */

text *
chkpass_rout(chkpass *password)
{
	text	   *result = NULL;

	if (password == NULL)
		return (NULL);

	if ((result = (text *) palloc(VARHDRSZ + 16)) != NULL)
	{
		VARSIZE(result) = VARHDRSZ + strlen(password->password);
		memcpy(VARDATA(result), password->password, strlen(password->password));
	}

	return (result);
}


/*
 * Boolean tests
 */

bool
chkpass_eq(chkpass * a1, text *a2)
{
	char	str[10];
	int		sz = 8;

	if (!a1 || !a2) return 0;
	if (a2->vl_len < 12) sz = a2->vl_len - 4;
	strncpy(str, a2->vl_dat, sz);
	str[sz] = 0;
	return (strcmp(a1->password, crypt(str, a1->password)) == 0);
}

bool
chkpass_ne(chkpass * a1, text *a2)
{
	char	str[10];
	int		sz = 8;

	if (!a1 || !a2) return 0;
	if (a2->vl_len < 12) sz = a2->vl_len - 4;
	strncpy(str, a2->vl_dat, sz);
	str[sz] = 0;
	return (strcmp(a1->password, crypt(str, a1->password)) != 0);
}

