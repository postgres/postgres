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

#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/xact.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_collation_fn.h"
#include "commands/alter.h"
#include "commands/collationcmds.h"
#include "commands/comment.h"
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
	DefElem    *providerEl = NULL;
	DefElem    *versionEl = NULL;
	char	   *collcollate = NULL;
	char	   *collctype = NULL;
	char	   *collproviderstr = NULL;
	int			collencoding;
	char		collprovider = 0;
	char	   *collversion = NULL;
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
		else if (pg_strcasecmp(defel->defname, "provider") == 0)
			defelp = &providerEl;
		else if (pg_strcasecmp(defel->defname, "version") == 0)
			defelp = &versionEl;
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
		collprovider = ((Form_pg_collation) GETSTRUCT(tp))->collprovider;

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

	if (providerEl)
		collproviderstr = defGetString(providerEl);

	if (versionEl)
		collversion = defGetString(versionEl);

	if (collproviderstr)
	{
		if (pg_strcasecmp(collproviderstr, "icu") == 0)
			collprovider = COLLPROVIDER_ICU;
		else if (pg_strcasecmp(collproviderstr, "libc") == 0)
			collprovider = COLLPROVIDER_LIBC;
		else
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("unrecognized collation provider: %s",
							collproviderstr)));
	}
	else if (!fromEl)
		collprovider = COLLPROVIDER_LIBC;

	if (!collcollate)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("parameter \"lc_collate\" must be specified")));

	if (!collctype)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("parameter \"lc_ctype\" must be specified")));

	if (collprovider == COLLPROVIDER_ICU)
		collencoding = -1;
	else
	{
		collencoding = GetDatabaseEncoding();
		check_encoding_locale_matches(collencoding, collcollate, collctype);
	}

	if (!collversion)
		collversion = get_collation_actual_version(collprovider, collcollate);

	newoid = CollationCreate(collName,
							 collNamespace,
							 GetUserId(),
							 collprovider,
							 collencoding,
							 collcollate,
							 collctype,
							 collversion,
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
 * ALTER COLLATION
 */
ObjectAddress
AlterCollation(AlterCollationStmt *stmt)
{
	Relation	rel;
	Oid			collOid;
	HeapTuple	tup;
	Form_pg_collation collForm;
	Datum		collversion;
	bool		isnull;
	char	   *oldversion;
	char	   *newversion;
	ObjectAddress address;

	rel = heap_open(CollationRelationId, RowExclusiveLock);
	collOid = get_collation_oid(stmt->collname, false);

	if (!pg_collation_ownercheck(collOid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_COLLATION,
					   NameListToString(stmt->collname));

	tup = SearchSysCacheCopy1(COLLOID, ObjectIdGetDatum(collOid));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for collation %u", collOid);

	collForm = (Form_pg_collation) GETSTRUCT(tup);
	collversion = SysCacheGetAttr(COLLOID, tup, Anum_pg_collation_collversion,
								  &isnull);
	oldversion = isnull ? NULL : TextDatumGetCString(collversion);

	newversion = get_collation_actual_version(collForm->collprovider, NameStr(collForm->collcollate));

	/* cannot change from NULL to non-NULL or vice versa */
	if ((!oldversion && newversion) || (oldversion && !newversion))
		elog(ERROR, "invalid collation version change");
	else if (oldversion && newversion && strcmp(newversion, oldversion) != 0)
	{
		bool        nulls[Natts_pg_collation];
		bool        replaces[Natts_pg_collation];
		Datum       values[Natts_pg_collation];

		ereport(NOTICE,
				(errmsg("changing version from %s to %s",
						oldversion, newversion)));

		memset(values, 0, sizeof(values));
		memset(nulls, false, sizeof(nulls));
		memset(replaces, false, sizeof(replaces));

		values[Anum_pg_collation_collversion - 1] = CStringGetTextDatum(newversion);
		replaces[Anum_pg_collation_collversion - 1] = true;

		tup = heap_modify_tuple(tup, RelationGetDescr(rel),
								values, nulls, replaces);
	}
	else
		ereport(NOTICE,
				(errmsg("version has not changed")));

	CatalogTupleUpdate(rel, &tup->t_self, tup);

	InvokeObjectPostAlterHook(CollationRelationId, collOid, 0);

	ObjectAddressSet(address, CollationRelationId, collOid);

	heap_freetuple(tup);
	heap_close(rel, NoLock);

	return address;
}


Datum
pg_collation_actual_version(PG_FUNCTION_ARGS)
{
	Oid			collid = PG_GETARG_OID(0);
	HeapTuple	tp;
	char	   *collcollate;
	char		collprovider;
	char	   *version;

	tp = SearchSysCache1(COLLOID, ObjectIdGetDatum(collid));
	if (!HeapTupleIsValid(tp))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("collation with OID %u does not exist", collid)));

	collcollate = pstrdup(NameStr(((Form_pg_collation) GETSTRUCT(tp))->collcollate));
	collprovider = ((Form_pg_collation) GETSTRUCT(tp))->collprovider;

	ReleaseSysCache(tp);

	version = get_collation_actual_version(collprovider, collcollate);

	if (version)
		PG_RETURN_TEXT_P(cstring_to_text(version));
	else
		PG_RETURN_NULL();
}


/*
 * "Normalize" a libc locale name, stripping off encoding tags such as
 * ".utf8" (e.g., "en_US.utf8" -> "en_US", but "br_FR.iso885915@euro"
 * -> "br_FR@euro").  Return true if a new, different name was
 * generated.
 */
pg_attribute_unused()
static bool
normalize_libc_locale_name(char *new, const char *old)
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


#ifdef USE_ICU
static char *
get_icu_language_tag(const char *localename)
{
	char		buf[ULOC_FULLNAME_CAPACITY];
	UErrorCode	status;

	status = U_ZERO_ERROR;
	uloc_toLanguageTag(localename, buf, sizeof(buf), TRUE, &status);
	if (U_FAILURE(status))
		ereport(ERROR,
				(errmsg("could not convert locale name \"%s\" to language tag: %s",
						localename, u_errorName(status))));

	return pstrdup(buf);
}


static char *
get_icu_locale_comment(const char *localename)
{
	UErrorCode	status;
	UChar		displayname[128];
	int32		len_uchar;
	char	   *result;

	status = U_ZERO_ERROR;
	len_uchar = uloc_getDisplayName(localename, "en", &displayname[0], sizeof(displayname), &status);
	if (U_FAILURE(status))
		ereport(ERROR,
				(errmsg("could get display name for locale \"%s\": %s",
						localename, u_errorName(status))));

	icu_from_uchar(&result, displayname, len_uchar);

	return result;
}
#endif	/* USE_ICU */


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

		CollationCreate(localebuf, nspid, GetUserId(), COLLPROVIDER_LIBC, enc,
						localebuf, localebuf,
						get_collation_actual_version(COLLPROVIDER_LIBC, localebuf),
						if_not_exists);

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
		if (normalize_libc_locale_name(alias, localebuf))
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

		CollationCreate(alias, nspid, GetUserId(), COLLPROVIDER_LIBC, enc,
						locale, locale,
						get_collation_actual_version(COLLPROVIDER_LIBC, locale),
						true);
		CommandCounterIncrement();
	}

	if (count == 0)
		ereport(WARNING,
				(errmsg("no usable system locales were found")));
#endif   /* not HAVE_LOCALE_T && not WIN32 */

#ifdef USE_ICU
	if (!is_encoding_supported_by_icu(GetDatabaseEncoding()))
	{
		ereport(NOTICE,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("encoding \"%s\" not supported by ICU",
						pg_encoding_to_char(GetDatabaseEncoding()))));
	}
	else
	{
		int i;

		/*
		 * Start the loop at -1 to sneak in the root locale without too much
		 * code duplication.
		 */
		for (i = -1; i < ucol_countAvailable(); i++)
		{
			const char *name;
			char	   *langtag;
			const char *collcollate;
			UEnumeration *en;
			UErrorCode	status;
			const char *val;
			Oid			collid;

			if (i == -1)
				name = "";  /* ICU root locale */
			else
				name = ucol_getAvailable(i);

			langtag = get_icu_language_tag(name);
			collcollate = U_ICU_VERSION_MAJOR_NUM >= 54 ? langtag : name;
			collid = CollationCreate(psprintf("%s-x-icu", langtag),
									 nspid, GetUserId(), COLLPROVIDER_ICU, -1,
									 collcollate, collcollate,
									 get_collation_actual_version(COLLPROVIDER_ICU, collcollate),
									 if_not_exists);

			CreateComments(collid, CollationRelationId, 0,
						   get_icu_locale_comment(name));

			/*
			 * Add keyword variants
			 */
			status = U_ZERO_ERROR;
			en = ucol_getKeywordValuesForLocale("collation", name, TRUE, &status);
			if (U_FAILURE(status))
				ereport(ERROR,
						(errmsg("could not get keyword values for locale \"%s\": %s",
								name, u_errorName(status))));

			status = U_ZERO_ERROR;
			uenum_reset(en, &status);
			while ((val = uenum_next(en, NULL, &status)))
			{
				char *localeid = psprintf("%s@collation=%s", name, val);

				langtag =  get_icu_language_tag(localeid);
				collcollate = U_ICU_VERSION_MAJOR_NUM >= 54 ? langtag : localeid;
				collid = CollationCreate(psprintf("%s-x-icu", langtag),
										 nspid, GetUserId(), COLLPROVIDER_ICU, -1,
										 collcollate, collcollate,
										 get_collation_actual_version(COLLPROVIDER_ICU, collcollate),
										 if_not_exists);
				CreateComments(collid, CollationRelationId, 0,
							   get_icu_locale_comment(localeid));
			}
			if (U_FAILURE(status))
				ereport(ERROR,
						(errmsg("could not get keyword values for locale \"%s\": %s",
								name, u_errorName(status))));
			uenum_close(en);
		}
	}
#endif

	PG_RETURN_VOID();
}
