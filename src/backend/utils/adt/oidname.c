/*-------------------------------------------------------------------------
 *
 * oidname.c--
 *	  adt for multiple key indices involving oid and name.	Used for cache
 *	  index scans (could also be used in the general case with name).
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/adt/Attic/oidname.c,v 1.7 1997/09/08 21:48:37 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <stdio.h>
#include <string.h>

#include "postgres.h"
#include "utils/oidcompos.h"	/* where function declarations go */
#include "utils/builtins.h"		/* for pg_atoi() */
#include "utils/palloc.h"

OidName
oidnamein(char *inStr)
{
	OidName		oc;
	char	   *inptr;

	oc = (OidName) palloc(sizeof(OidNameData));

	memset(oc, 0, sizeof(OidNameData));
	for (inptr = inStr; *inptr && *inptr != ','; inptr++)
		;

	if (*inptr)
	{
		oc->id = (Oid) pg_atoi(inStr, sizeof(Oid), ',');
		/* copy one less to ensure null-padding */
		++inptr;
		strNcpy(oc->name.data, inptr, NAMEDATALEN - 1);
	}
	else
		elog(WARN, "Bad input data for type oidname");

	return oc;
}

char	   *
oidnameout(OidName oidname)
{
	char		buf[30 + NAMEDATALEN];	/* oidname length + oid length +
										 * some safety */
	char	   *res;

	sprintf(buf, "%d,%s", oidname->id, oidname->name.data);
	res = pstrdup(buf);
	return (res);
}

bool
oidnamelt(OidName o1, OidName o2)
{
	return (bool)
	(o1->id < o2->id ||
	 (o1->id == o2->id && namecmp(&o1->name, &o2->name) < 0));
}

bool
oidnamele(OidName o1, OidName o2)
{
	return (bool)
	(o1->id < o2->id ||
	 (o1->id == o2->id && namecmp(&o1->name, &o2->name) <= 0));
}

bool
oidnameeq(OidName o1, OidName o2)
{
	return (bool)
	(o1->id == o2->id &&
	 (namecmp(&o1->name, &o2->name) == 0));
}

bool
oidnamene(OidName o1, OidName o2)
{
	return (bool)
	(o1->id != o2->id ||
	 (namecmp(&o1->name, &o2->name) != 0));
}

bool
oidnamege(OidName o1, OidName o2)
{
	return (bool) (o1->id > o2->id || (o1->id == o2->id &&
									namecmp(&o1->name, &o2->name) >= 0));
}

bool
oidnamegt(OidName o1, OidName o2)
{
	return (bool) (o1->id > o2->id || (o1->id == o2->id &&
									 namecmp(&o1->name, &o2->name) > 0));
}

int
oidnamecmp(OidName o1, OidName o2)
{
	if (o1->id == o2->id)
		return (namecmp(&o1->name, &o2->name));

	return (o1->id < o2->id) ? -1 : 1;
}

OidName
mkoidname(Oid id, char *name)
{
	OidName		oidname;

	oidname = (OidName) palloc(sizeof(Oid) + NAMEDATALEN);

	oidname->id = id;
	namestrcpy(&oidname->name, name);
	return oidname;
}
