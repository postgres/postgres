/*
 * PostgreSQL type definitions for chkpass
 * Written by D'Arcy J.M. Cain
 * darcy@druid.net
 * http://www.druid.net/darcy/
 *
 * $PostgreSQL: pgsql/contrib/chkpass/chkpass.c,v 1.21 2009/06/11 14:48:50 momjian Exp $
 * best viewed with tabs set to 4
 */

#include "postgres.h"

#include <time.h>
#include <unistd.h>
#ifdef HAVE_CRYPT_H
#include <crypt.h>
#endif

#include "fmgr.h"
#include "utils/builtins.h"

PG_MODULE_MAGIC;

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
	char		password[16];
} chkpass;

/*
 * Various forward declarations:
 */

Datum		chkpass_in(PG_FUNCTION_ARGS);
Datum		chkpass_out(PG_FUNCTION_ARGS);
Datum		chkpass_rout(PG_FUNCTION_ARGS);

/* Only equal or not equal make sense */
Datum		chkpass_eq(PG_FUNCTION_ARGS);
Datum		chkpass_ne(PG_FUNCTION_ARGS);


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
PG_FUNCTION_INFO_V1(chkpass_in);
Datum
chkpass_in(PG_FUNCTION_ARGS)
{
	char	   *str = PG_GETARG_CSTRING(0);
	chkpass    *result;
	char		mysalt[4];
	static char salt_chars[] =
	"./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";

	/* special case to let us enter encrypted passwords */
	if (*str == ':')
	{
		result = (chkpass *) palloc(sizeof(chkpass));
		strlcpy(result->password, str + 1, 13 + 1);
		PG_RETURN_POINTER(result);
	}

	if (verify_pass(str) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("password \"%s\" is weak", str)));

	result = (chkpass *) palloc(sizeof(chkpass));

	mysalt[0] = salt_chars[random() & 0x3f];
	mysalt[1] = salt_chars[random() & 0x3f];
	mysalt[2] = 0;				/* technically the terminator is not necessary
								 * but I like to play safe */
	strcpy(result->password, crypt(str, mysalt));
	PG_RETURN_POINTER(result);
}

/*
 * CHKPASS output function.
 * Just like any string but we know it is max 15 (13 plus colon and terminator.)
 */

PG_FUNCTION_INFO_V1(chkpass_out);
Datum
chkpass_out(PG_FUNCTION_ARGS)
{
	chkpass    *password = (chkpass *) PG_GETARG_POINTER(0);
	char	   *result;

	result = (char *) palloc(16);
	result[0] = ':';
	strcpy(result + 1, password->password);

	PG_RETURN_CSTRING(result);
}


/*
 * special output function that doesn't output the colon
 */

PG_FUNCTION_INFO_V1(chkpass_rout);
Datum
chkpass_rout(PG_FUNCTION_ARGS)
{
	chkpass    *password = (chkpass *) PG_GETARG_POINTER(0);

	PG_RETURN_TEXT_P(cstring_to_text(password->password));
}


/*
 * Boolean tests
 */

PG_FUNCTION_INFO_V1(chkpass_eq);
Datum
chkpass_eq(PG_FUNCTION_ARGS)
{
	chkpass    *a1 = (chkpass *) PG_GETARG_POINTER(0);
	text	   *a2 = PG_GETARG_TEXT_PP(1);
	char		str[9];

	text_to_cstring_buffer(a2, str, sizeof(str));
	PG_RETURN_BOOL(strcmp(a1->password, crypt(str, a1->password)) == 0);
}

PG_FUNCTION_INFO_V1(chkpass_ne);
Datum
chkpass_ne(PG_FUNCTION_ARGS)
{
	chkpass    *a1 = (chkpass *) PG_GETARG_POINTER(0);
	text	   *a2 = PG_GETARG_TEXT_PP(1);
	char		str[9];

	text_to_cstring_buffer(a2, str, sizeof(str));
	PG_RETURN_BOOL(strcmp(a1->password, crypt(str, a1->password)) != 0);
}
