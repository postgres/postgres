/*-------------------------------------------------------------------------
 *
 * collationcmds.c
 *	  collation-related commands support code
 *
 * Portions Copyright (c) 1996-2017, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/collationcmds.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/xact.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_collation_fn.h"
#include "commands/alter.h"
#include "commands/collationcmds.h"
#include "commands/dbcommands.h"
#include "commands/defrem.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/pg_locale.h"
#include "utils/rel.h"
#include "utils/syscache.h"

/*
 * CREATE COLLATION
 */
ObjectAddress
DefineCollation(ParseState *pstate, List *names, List *parameters, bool if_not_exists)
{
	char	   *collName;
	Oid			collNamespace;
	AclResult	aclresult;
	ListCell   *pl;
	DefElem    *fromEl = NULL;
	DefElem    *localeEl = NULL;
	DefElem    *lccollateEl = NULL;
	DefElem    *lcctypeEl = NULL;
	char	   *collcollate = NULL;
	char	   *collctype = NULL;
	Oid			newoid;
	ObjectAddress address;

	collNamespace = QualifiedNameGetCreationNamespace(names, &collName);

	aclresult = pg_namespace_aclcheck(collNamespace, GetUserId(), ACL_CREATE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, ACL_KIND_NAMESPACE,
					   get_namespace_name(collNamespace));

	foreach(pl, parameters)
	{
		DefElem    *defel = castNode(DefElem, lfirst(pl));
		DefElem   **defelp;

		if (pg_strcasecmp(defel->defname, "from") == 0)
			defelp = &fromEl;
		else if (pg_strcasecmp(defel->defname, "locale") == 0)
			defelp = &localeEl;
		else if (pg_strcasecmp(defel->defname, "lc_collate") == 0)
			defelp = &lccollateEl;
		else if (pg_strcasecmp(defel->defname, "lc_ctype") == 0)
			defelp = &lcctypeEl;
		else
		{
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("collation attribute \"%s\" not recognized",
							defel->defname),
					 parser_errposition(pstate, defel->location)));
			break;
		}

		*defelp = defel;
	}

	if ((localeEl && (lccollateEl || lcctypeEl))
		|| (fromEl && list_length(parameters) != 1))
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("conflicting or redundant options")));

	if (fromEl)
	{
		Oid			collid;
		HeapTuple	tp;

		collid = get_collation_oid(defGetQualifiedName(fromEl), false);
		tp = SearchSysCache1(COLLOID, ObjectIdGetDatum(collid));
		if (!HeapTupleIsValid(tp))
			elog(ERROR, "cache lookup failed for collation %u", collid);

		collcollate = pstrdup(NameStr(((Form_pg_collation) GETSTRUCT(tp))->collcollate));
		collctype = pstrdup(NameStr(((Form_pg_collation) GETSTRUCT(tp))->collctype));

		ReleaseSysCache(tp);
	}

	if (localeEl)
	{
		collcollate = defGetString(localeEl);
		collctype = defGetString(localeEl);
	}

	if (lccollateEl)
		collcollate = defGetString(lccollateEl);

	if (lcctypeEl)
		collctype = defGetString(lcctypeEl);

	if (!collcollate)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("parameter \"lc_collate\" must be specified")));

	if (!collctype)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("parameter \"lc_ctype\" must be specified")));

	check_encoding_locale_matches(GetDatabaseEncoding(), collcollate, collctype);

	newoid = CollationCreate(collName,
							 collNamespace,
							 GetUserId(),
							 GetDatabaseEncoding(),
							 collcollate,
							 collctype,
							 if_not_exists);

	if (!OidIsValid(newoid))
		return InvalidObjectAddress;

	ObjectAddressSet(address, CollationRelationId, newoid);

	/* check that the locales can be loaded */
	CommandCounterIncrement();
	(void) pg_newlocale_from_collation(newoid);

	return address;
}

/*
 * Subroutine for ALTER COLLATION SET SCHEMA and RENAME
 *
 * Is there a collation with the same name of the given collation already in
 * the given namespace?  If so, raise an appropriate error message.
 */
void
IsThereCollationInNamespace(const char *collname, Oid nspOid)
{
	/* make sure the name doesn't already exist in new schema */
	if (SearchSysCacheExists3(COLLNAMEENCNSP,
							  CStringGetDatum(collname),
							  Int32GetDatum(GetDatabaseEncoding()),
							  ObjectIdGetDatum(nspOid)))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("collation \"%s\" for encoding \"%s\" already exists in schema \"%s\"",
						collname, GetDatabaseEncodingName(),
						get_namespace_name(nspOid))));

	/* mustn't match an any-encoding entry, either */
	if (SearchSysCacheExists3(COLLNAMEENCNSP,
							  CStringGetDatum(collname),
							  Int32GetDatum(-1),
							  ObjectIdGetDatum(nspOid)))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("collation \"%s\" already exists in schema \"%s\"",
						collname, get_namespace_name(nspOid))));
}


/*
 * "Normalize" a locale name, stripping off encoding tags such as
 * ".utf8" (e.g., "en_US.utf8" -> "en_US", but "br_FR.iso885915@euro"
 * -> "br_FR@euro").  Return true if a new, different name was
 * generated.
 */
pg_attribute_unused()
static bool
normalize_locale_name(char *new, const char *old)
{
	char	   *n = new;
	const char *o = old;
	bool		changed = false;

	while (*o)
	{
		if (*o == '.')
		{
			/* skip over encoding tag such as ".utf8" or ".UTF-8" */
			o++;
			while ((*o >= 'A' && *o <= 'Z')
				   || (*o >= 'a' && *o <= 'z')
				   || (*o >= '0' && *o <= '9')
				   || (*o == '-'))
				o++;
			changed = true;
		}
		else
			*n++ = *o++;
	}
	*n = '\0';

	return changed;
}


Datum
pg_import_system_collations(PG_FUNCTION_ARGS)
{
#if defined(HAVE_LOCALE_T) && !defined(WIN32)
	bool		if_not_exists = PG_GETARG_BOOL(0);
	Oid			nspid = PG_GETARG_OID(1);

	FILE	   *locale_a_handle;
	char		localebuf[NAMEDATALEN]; /* we assume ASCII so this is fine */
	int			count = 0;
	List	   *aliaslist = NIL;
	List	   *localelist = NIL;
	List	   *enclist = NIL;
	ListCell   *lca,
			   *lcl,
			   *lce;
#endif

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 (errmsg("must be superuser to import system collations"))));

#if defined(HAVE_LOCALE_T) && !defined(WIN32)
	locale_a_handle = OpenPipeStream("locale -a", "r");
	if (locale_a_handle == NULL)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not execute command \"%s\": %m",
						"locale -a")));

	while (fgets(localebuf, sizeof(localebuf), locale_a_handle))
	{
		int			i;
		size_t		len;
		int			enc;
		bool		skip;
		char		alias[NAMEDATALEN];

		len = strlen(localebuf);

		if (len == 0 || localebuf[len - 1] != '\n')
		{
			elog(DEBUG1, "locale name too long, skipped: \"%s\"", localebuf);
			continue;
		}
		localebuf[len - 1] = '\0';

		/*
		 * Some systems have locale names that don't consist entirely of ASCII
		 * letters (such as "bokm&aring;l" or "fran&ccedil;ais").  This is
		 * pretty silly, since we need the locale itself to interpret the
		 * non-ASCII characters. We can't do much with those, so we filter
		 * them out.
		 */
		skip = false;
		for (i = 0; i < len; i++)
		{
			if (IS_HIGHBIT_SET(localebuf[i]))
			{
				skip = true;
				break;
			}
		}
		if (skip)
		{
			elog(DEBUG1, "locale name has non-ASCII characters, skipped: \"%s\"", localebuf);
			continue;
		}

		enc = pg_get_encoding_from_locale(localebuf, false);
		if (enc < 0)
		{
			/* error message printed by pg_get_encoding_from_locale() */
			continue;
		}
		if (!PG_VALID_BE_ENCODING(enc))
			continue;			/* ignore locales for client-only encodings */
		if (enc == PG_SQL_ASCII)
			continue;			/* C/POSIX are already in the catalog */

		count++;

		CollationCreate(localebuf, nspid, GetUserId(), enc,
						localebuf, localebuf, if_not_exists);

		CommandCounterIncrement();

		/*
		 * Generate aliases such as "en_US" in addition to "en_US.utf8" for
		 * ease of use.  Note that collation names are unique per encoding
		 * only, so this doesn't clash with "en_US" for LATIN1, say.
		 *
		 * However, it might conflict with a name we'll see later in the
		 * "locale -a" output.  So save up the aliases and try to add them
		 * after we've read all the output.
		 */
		if (normalize_locale_name(alias, localebuf))
		{
			aliaslist = lappend(aliaslist, pstrdup(alias));
			localelist = lappend(localelist, pstrdup(localebuf));
			enclist = lappend_int(enclist, enc);
		}
	}

	ClosePipeStream(locale_a_handle);

	/* Now try to add any aliases we created */
	forthree(lca, aliaslist, lcl, localelist, lce, enclist)
	{
		char	   *alias = (char *) lfirst(lca);
		char	   *locale = (char *) lfirst(lcl);
		int			enc = lfirst_int(lce);

		CollationCreate(alias, nspid, GetUserId(), enc,
						locale, locale, true);
		CommandCounterIncrement();
	}

	if (count == 0)
		ereport(WARNING,
				(errmsg("no usable system locales were found")));
#endif   /* not HAVE_LOCALE_T && not WIN32 */

	PG_RETURN_VOID();
}
