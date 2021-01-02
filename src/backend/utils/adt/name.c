/*-------------------------------------------------------------------------
 *
 * name.c
 *	  Functions for the built-in type "name".
 *
 * name replaces char16 and is carefully implemented so that it
 * is a string of physical length NAMEDATALEN.
 * DO NOT use hard-coded constants anywhere
 * always use NAMEDATALEN as the symbolic constant!   - jolly 8/21/95
 *
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/name.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/namespace.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_type.h"
#include "libpq/pqformat.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/varlena.h"


/*****************************************************************************
 *	 USER I/O ROUTINES (none)												 *
 *****************************************************************************/


/*
 *		namein	- converts "..." to internal representation
 *
 *		Note:
 *				[Old] Currently if strlen(s) < NAMEDATALEN, the extra chars are nulls
 *				Now, always NULL terminated
 */
Datum
namein(PG_FUNCTION_ARGS)
{
	char	   *s = PG_GETARG_CSTRING(0);
	Name		result;
	int			len;

	len = strlen(s);

	/* Truncate oversize input */
	if (len >= NAMEDATALEN)
		len = pg_mbcliplen(s, len, NAMEDATALEN - 1);

	/* We use palloc0 here to ensure result is zero-padded */
	result = (Name) palloc0(NAMEDATALEN);
	memcpy(NameStr(*result), s, len);

	PG_RETURN_NAME(result);
}

/*
 *		nameout - converts internal representation to "..."
 */
Datum
nameout(PG_FUNCTION_ARGS)
{
	Name		s = PG_GETARG_NAME(0);

	PG_RETURN_CSTRING(pstrdup(NameStr(*s)));
}

/*
 *		namerecv			- converts external binary format to name
 */
Datum
namerecv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);
	Name		result;
	char	   *str;
	int			nbytes;

	str = pq_getmsgtext(buf, buf->len - buf->cursor, &nbytes);
	if (nbytes >= NAMEDATALEN)
		ereport(ERROR,
				(errcode(ERRCODE_NAME_TOO_LONG),
				 errmsg("identifier too long"),
				 errdetail("Identifier must be less than %d characters.",
						   NAMEDATALEN)));
	result = (NameData *) palloc0(NAMEDATALEN);
	memcpy(result, str, nbytes);
	pfree(str);
	PG_RETURN_NAME(result);
}

/*
 *		namesend			- converts name to binary format
 */
Datum
namesend(PG_FUNCTION_ARGS)
{
	Name		s = PG_GETARG_NAME(0);
	StringInfoData buf;

	pq_begintypsend(&buf);
	pq_sendtext(&buf, NameStr(*s), strlen(NameStr(*s)));
	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}


/*****************************************************************************
 *	 COMPARISON/SORTING ROUTINES											 *
 *****************************************************************************/

/*
 *		nameeq	- returns 1 iff arguments are equal
 *		namene	- returns 1 iff arguments are not equal
 *		namelt	- returns 1 iff a < b
 *		namele	- returns 1 iff a <= b
 *		namegt	- returns 1 iff a > b
 *		namege	- returns 1 iff a >= b
 *
 * Note that the use of strncmp with NAMEDATALEN limit is mostly historical;
 * strcmp would do as well, because we do not allow NAME values that don't
 * have a '\0' terminator.  Whatever might be past the terminator is not
 * considered relevant to comparisons.
 */
static int
namecmp(Name arg1, Name arg2, Oid collid)
{
	/* Fast path for common case used in system catalogs */
	if (collid == C_COLLATION_OID)
		return strncmp(NameStr(*arg1), NameStr(*arg2), NAMEDATALEN);

	/* Else rely on the varstr infrastructure */
	return varstr_cmp(NameStr(*arg1), strlen(NameStr(*arg1)),
					  NameStr(*arg2), strlen(NameStr(*arg2)),
					  collid);
}

Datum
nameeq(PG_FUNCTION_ARGS)
{
	Name		arg1 = PG_GETARG_NAME(0);
	Name		arg2 = PG_GETARG_NAME(1);

	PG_RETURN_BOOL(namecmp(arg1, arg2, PG_GET_COLLATION()) == 0);
}

Datum
namene(PG_FUNCTION_ARGS)
{
	Name		arg1 = PG_GETARG_NAME(0);
	Name		arg2 = PG_GETARG_NAME(1);

	PG_RETURN_BOOL(namecmp(arg1, arg2, PG_GET_COLLATION()) != 0);
}

Datum
namelt(PG_FUNCTION_ARGS)
{
	Name		arg1 = PG_GETARG_NAME(0);
	Name		arg2 = PG_GETARG_NAME(1);

	PG_RETURN_BOOL(namecmp(arg1, arg2, PG_GET_COLLATION()) < 0);
}

Datum
namele(PG_FUNCTION_ARGS)
{
	Name		arg1 = PG_GETARG_NAME(0);
	Name		arg2 = PG_GETARG_NAME(1);

	PG_RETURN_BOOL(namecmp(arg1, arg2, PG_GET_COLLATION()) <= 0);
}

Datum
namegt(PG_FUNCTION_ARGS)
{
	Name		arg1 = PG_GETARG_NAME(0);
	Name		arg2 = PG_GETARG_NAME(1);

	PG_RETURN_BOOL(namecmp(arg1, arg2, PG_GET_COLLATION()) > 0);
}

Datum
namege(PG_FUNCTION_ARGS)
{
	Name		arg1 = PG_GETARG_NAME(0);
	Name		arg2 = PG_GETARG_NAME(1);

	PG_RETURN_BOOL(namecmp(arg1, arg2, PG_GET_COLLATION()) >= 0);
}

Datum
btnamecmp(PG_FUNCTION_ARGS)
{
	Name		arg1 = PG_GETARG_NAME(0);
	Name		arg2 = PG_GETARG_NAME(1);

	PG_RETURN_INT32(namecmp(arg1, arg2, PG_GET_COLLATION()));
}

Datum
btnamesortsupport(PG_FUNCTION_ARGS)
{
	SortSupport ssup = (SortSupport) PG_GETARG_POINTER(0);
	Oid			collid = ssup->ssup_collation;
	MemoryContext oldcontext;

	oldcontext = MemoryContextSwitchTo(ssup->ssup_cxt);

	/* Use generic string SortSupport */
	varstr_sortsupport(ssup, NAMEOID, collid);

	MemoryContextSwitchTo(oldcontext);

	PG_RETURN_VOID();
}


/*****************************************************************************
 *	 MISCELLANEOUS PUBLIC ROUTINES											 *
 *****************************************************************************/

void
namestrcpy(Name name, const char *str)
{
	/* NB: We need to zero-pad the destination. */
	strncpy(NameStr(*name), str, NAMEDATALEN);
	NameStr(*name)[NAMEDATALEN-1] = '\0';
}

/*
 * Compare a NAME to a C string
 *
 * Assumes C collation always; be careful when using this for
 * anything but equality checks!
 */
int
namestrcmp(Name name, const char *str)
{
	if (!name && !str)
		return 0;
	if (!name)
		return -1;				/* NULL < anything */
	if (!str)
		return 1;				/* NULL < anything */
	return strncmp(NameStr(*name), str, NAMEDATALEN);
}


/*
 * SQL-functions CURRENT_USER, SESSION_USER
 */
Datum
current_user(PG_FUNCTION_ARGS)
{
	PG_RETURN_DATUM(DirectFunctionCall1(namein, CStringGetDatum(GetUserNameFromId(GetUserId(), false))));
}

Datum
session_user(PG_FUNCTION_ARGS)
{
	PG_RETURN_DATUM(DirectFunctionCall1(namein, CStringGetDatum(GetUserNameFromId(GetSessionUserId(), false))));
}


/*
 * SQL-functions CURRENT_SCHEMA, CURRENT_SCHEMAS
 */
Datum
current_schema(PG_FUNCTION_ARGS)
{
	List	   *search_path = fetch_search_path(false);
	char	   *nspname;

	if (search_path == NIL)
		PG_RETURN_NULL();
	nspname = get_namespace_name(linitial_oid(search_path));
	list_free(search_path);
	if (!nspname)
		PG_RETURN_NULL();		/* recently-deleted namespace? */
	PG_RETURN_DATUM(DirectFunctionCall1(namein, CStringGetDatum(nspname)));
}

Datum
current_schemas(PG_FUNCTION_ARGS)
{
	List	   *search_path = fetch_search_path(PG_GETARG_BOOL(0));
	ListCell   *l;
	Datum	   *names;
	int			i;
	ArrayType  *array;

	names = (Datum *) palloc(list_length(search_path) * sizeof(Datum));
	i = 0;
	foreach(l, search_path)
	{
		char	   *nspname;

		nspname = get_namespace_name(lfirst_oid(l));
		if (nspname)			/* watch out for deleted namespace */
		{
			names[i] = DirectFunctionCall1(namein, CStringGetDatum(nspname));
			i++;
		}
	}
	list_free(search_path);

	array = construct_array(names, i,
							NAMEOID,
							NAMEDATALEN,	/* sizeof(Name) */
							false,	/* Name is not by-val */
							TYPALIGN_CHAR); /* alignment of Name */

	PG_RETURN_POINTER(array);
}

/*
 * SQL-function nameconcatoid(name, oid) returns name
 *
 * This is used in the information_schema to produce specific_name columns,
 * which are supposed to be unique per schema.  We achieve that (in an ugly
 * way) by appending the object's OID.  The result is the same as
 *		($1::text || '_' || $2::text)::name
 * except that, if it would not fit in NAMEDATALEN, we make it do so by
 * truncating the name input (not the oid).
 */
Datum
nameconcatoid(PG_FUNCTION_ARGS)
{
	Name		nam = PG_GETARG_NAME(0);
	Oid			oid = PG_GETARG_OID(1);
	Name		result;
	char		suffix[20];
	int			suflen;
	int			namlen;

	suflen = snprintf(suffix, sizeof(suffix), "_%u", oid);
	namlen = strlen(NameStr(*nam));

	/* Truncate oversize input by truncating name part, not suffix */
	if (namlen + suflen >= NAMEDATALEN)
		namlen = pg_mbcliplen(NameStr(*nam), namlen, NAMEDATALEN - 1 - suflen);

	/* We use palloc0 here to ensure result is zero-padded */
	result = (Name) palloc0(NAMEDATALEN);
	memcpy(NameStr(*result), NameStr(*nam), namlen);
	memcpy(NameStr(*result) + namlen, suffix, suflen);

	PG_RETURN_NAME(result);
}
