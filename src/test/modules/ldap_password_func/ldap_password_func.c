/*-------------------------------------------------------------------------
 *
 * Copyright (c) 2022-2024, PostgreSQL Global Development Group
 *
 * ldap_password_func.c
 *
 * Loadable PostgreSQL module to mutate the ldapbindpasswd. This
 * implementation just hands back the configured password rot13'd.
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <float.h>
#include <stdio.h>

#include "fmgr.h"
#include "libpq/auth.h"

PG_MODULE_MAGIC;

void		_PG_init(void);

/* hook function */
static char *rot13_passphrase(char *password);

/*
 * Module load callback
 */
void
_PG_init(void)
{
	ldap_password_hook = rot13_passphrase;
}

static char *
rot13_passphrase(char *pw)
{
	size_t		size = strlen(pw) + 1;

	char	   *new_pw = (char *) palloc(size);

	strlcpy(new_pw, pw, size);
	for (char *p = new_pw; *p; p++)
	{
		char		c = *p;

		if ((c >= 'a' && c <= 'm') || (c >= 'A' && c <= 'M'))
			*p = c + 13;
		else if ((c >= 'n' && c <= 'z') || (c >= 'N' && c <= 'Z'))
			*p = c - 13;
	}

	return new_pw;
}
