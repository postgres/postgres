/*-------------------------------------------------------------------------
 *
 * hbafuncs.c
 *	  Support functions for SQL views of authentication files.
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/hbafuncs.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/objectaddress.h"
#include "common/ip.h"
#include "funcapi.h"
#include "libpq/hba.h"
#include "miscadmin.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/guc.h"


static ArrayType *get_hba_options(HbaLine *hba);
static void fill_hba_line(Tuplestorestate *tuple_store, TupleDesc tupdesc,
						  int lineno, HbaLine *hba, const char *err_msg);
static void fill_hba_view(Tuplestorestate *tuple_store, TupleDesc tupdesc);
static void fill_ident_line(Tuplestorestate *tuple_store, TupleDesc tupdesc,
							int lineno, IdentLine *ident, const char *err_msg);
static void fill_ident_view(Tuplestorestate *tuple_store, TupleDesc tupdesc);


/*
 * This macro specifies the maximum number of authentication options
 * that are possible with any given authentication method that is supported.
 * Currently LDAP supports 11, and there are 3 that are not dependent on
 * the auth method here.  It may not actually be possible to set all of them
 * at the same time, but we'll set the macro value high enough to be
 * conservative and avoid warnings from static analysis tools.
 */
#define MAX_HBA_OPTIONS 14

/*
 * Create a text array listing the options specified in the HBA line.
 * Return NULL if no options are specified.
 */
static ArrayType *
get_hba_options(HbaLine *hba)
{
	int			noptions;
	Datum		options[MAX_HBA_OPTIONS];

	noptions = 0;

	if (hba->auth_method == uaGSS || hba->auth_method == uaSSPI)
	{
		if (hba->include_realm)
			options[noptions++] =
				CStringGetTextDatum("include_realm=true");

		if (hba->krb_realm)
			options[noptions++] =
				CStringGetTextDatum(psprintf("krb_realm=%s", hba->krb_realm));
	}

	if (hba->usermap)
		options[noptions++] =
			CStringGetTextDatum(psprintf("map=%s", hba->usermap));

	if (hba->clientcert != clientCertOff)
		options[noptions++] =
			CStringGetTextDatum(psprintf("clientcert=%s", (hba->clientcert == clientCertCA) ? "verify-ca" : "verify-full"));

	if (hba->pamservice)
		options[noptions++] =
			CStringGetTextDatum(psprintf("pamservice=%s", hba->pamservice));

	if (hba->auth_method == uaLDAP)
	{
		if (hba->ldapserver)
			options[noptions++] =
				CStringGetTextDatum(psprintf("ldapserver=%s", hba->ldapserver));

		if (hba->ldapport)
			options[noptions++] =
				CStringGetTextDatum(psprintf("ldapport=%d", hba->ldapport));

		if (hba->ldaptls)
			options[noptions++] =
				CStringGetTextDatum("ldaptls=true");

		if (hba->ldapprefix)
			options[noptions++] =
				CStringGetTextDatum(psprintf("ldapprefix=%s", hba->ldapprefix));

		if (hba->ldapsuffix)
			options[noptions++] =
				CStringGetTextDatum(psprintf("ldapsuffix=%s", hba->ldapsuffix));

		if (hba->ldapbasedn)
			options[noptions++] =
				CStringGetTextDatum(psprintf("ldapbasedn=%s", hba->ldapbasedn));

		if (hba->ldapbinddn)
			options[noptions++] =
				CStringGetTextDatum(psprintf("ldapbinddn=%s", hba->ldapbinddn));

		if (hba->ldapbindpasswd)
			options[noptions++] =
				CStringGetTextDatum(psprintf("ldapbindpasswd=%s",
											 hba->ldapbindpasswd));

		if (hba->ldapsearchattribute)
			options[noptions++] =
				CStringGetTextDatum(psprintf("ldapsearchattribute=%s",
											 hba->ldapsearchattribute));

		if (hba->ldapsearchfilter)
			options[noptions++] =
				CStringGetTextDatum(psprintf("ldapsearchfilter=%s",
											 hba->ldapsearchfilter));

		if (hba->ldapscope)
			options[noptions++] =
				CStringGetTextDatum(psprintf("ldapscope=%d", hba->ldapscope));
	}

	if (hba->auth_method == uaRADIUS)
	{
		if (hba->radiusservers_s)
			options[noptions++] =
				CStringGetTextDatum(psprintf("radiusservers=%s", hba->radiusservers_s));

		if (hba->radiussecrets_s)
			options[noptions++] =
				CStringGetTextDatum(psprintf("radiussecrets=%s", hba->radiussecrets_s));

		if (hba->radiusidentifiers_s)
			options[noptions++] =
				CStringGetTextDatum(psprintf("radiusidentifiers=%s", hba->radiusidentifiers_s));

		if (hba->radiusports_s)
			options[noptions++] =
				CStringGetTextDatum(psprintf("radiusports=%s", hba->radiusports_s));
	}

	/* If you add more options, consider increasing MAX_HBA_OPTIONS. */
	Assert(noptions <= MAX_HBA_OPTIONS);

	if (noptions > 0)
		return construct_array(options, noptions, TEXTOID, -1, false, TYPALIGN_INT);
	else
		return NULL;
}

/* Number of columns in pg_hba_file_rules view */
#define NUM_PG_HBA_FILE_RULES_ATTS	 9

/*
 * fill_hba_line
 *		Build one row of pg_hba_file_rules view, add it to tuplestore.
 *
 * tuple_store: where to store data
 * tupdesc: tuple descriptor for the view
 * lineno: pg_hba.conf line number (must always be valid)
 * hba: parsed line data (can be NULL, in which case err_msg should be set)
 * err_msg: error message (NULL if none)
 *
 * Note: leaks memory, but we don't care since this is run in a short-lived
 * memory context.
 */
static void
fill_hba_line(Tuplestorestate *tuple_store, TupleDesc tupdesc,
			  int lineno, HbaLine *hba, const char *err_msg)
{
	Datum		values[NUM_PG_HBA_FILE_RULES_ATTS];
	bool		nulls[NUM_PG_HBA_FILE_RULES_ATTS];
	char		buffer[NI_MAXHOST];
	HeapTuple	tuple;
	int			index;
	ListCell   *lc;
	const char *typestr;
	const char *addrstr;
	const char *maskstr;
	ArrayType  *options;

	Assert(tupdesc->natts == NUM_PG_HBA_FILE_RULES_ATTS);

	memset(values, 0, sizeof(values));
	memset(nulls, 0, sizeof(nulls));
	index = 0;

	/* line_number */
	values[index++] = Int32GetDatum(lineno);

	if (hba != NULL)
	{
		/* type */
		/* Avoid a default: case so compiler will warn about missing cases */
		typestr = NULL;
		switch (hba->conntype)
		{
			case ctLocal:
				typestr = "local";
				break;
			case ctHost:
				typestr = "host";
				break;
			case ctHostSSL:
				typestr = "hostssl";
				break;
			case ctHostNoSSL:
				typestr = "hostnossl";
				break;
			case ctHostGSS:
				typestr = "hostgssenc";
				break;
			case ctHostNoGSS:
				typestr = "hostnogssenc";
				break;
		}
		if (typestr)
			values[index++] = CStringGetTextDatum(typestr);
		else
			nulls[index++] = true;

		/* database */
		if (hba->databases)
		{
			/*
			 * Flatten AuthToken list to string list.  It might seem that we
			 * should re-quote any quoted tokens, but that has been rejected
			 * on the grounds that it makes it harder to compare the array
			 * elements to other system catalogs.  That makes entries like
			 * "all" or "samerole" formally ambiguous ... but users who name
			 * databases/roles that way are inflicting their own pain.
			 */
			List	   *names = NIL;

			foreach(lc, hba->databases)
			{
				AuthToken  *tok = lfirst(lc);

				names = lappend(names, tok->string);
			}
			values[index++] = PointerGetDatum(strlist_to_textarray(names));
		}
		else
			nulls[index++] = true;

		/* user */
		if (hba->roles)
		{
			/* Flatten AuthToken list to string list; see comment above */
			List	   *roles = NIL;

			foreach(lc, hba->roles)
			{
				AuthToken  *tok = lfirst(lc);

				roles = lappend(roles, tok->string);
			}
			values[index++] = PointerGetDatum(strlist_to_textarray(roles));
		}
		else
			nulls[index++] = true;

		/* address and netmask */
		/* Avoid a default: case so compiler will warn about missing cases */
		addrstr = maskstr = NULL;
		switch (hba->ip_cmp_method)
		{
			case ipCmpMask:
				if (hba->hostname)
				{
					addrstr = hba->hostname;
				}
				else
				{
					/*
					 * Note: if pg_getnameinfo_all fails, it'll set buffer to
					 * "???", which we want to return.
					 */
					if (hba->addrlen > 0)
					{
						if (pg_getnameinfo_all(&hba->addr, hba->addrlen,
											   buffer, sizeof(buffer),
											   NULL, 0,
											   NI_NUMERICHOST) == 0)
							clean_ipv6_addr(hba->addr.ss_family, buffer);
						addrstr = pstrdup(buffer);
					}
					if (hba->masklen > 0)
					{
						if (pg_getnameinfo_all(&hba->mask, hba->masklen,
											   buffer, sizeof(buffer),
											   NULL, 0,
											   NI_NUMERICHOST) == 0)
							clean_ipv6_addr(hba->mask.ss_family, buffer);
						maskstr = pstrdup(buffer);
					}
				}
				break;
			case ipCmpAll:
				addrstr = "all";
				break;
			case ipCmpSameHost:
				addrstr = "samehost";
				break;
			case ipCmpSameNet:
				addrstr = "samenet";
				break;
		}
		if (addrstr)
			values[index++] = CStringGetTextDatum(addrstr);
		else
			nulls[index++] = true;
		if (maskstr)
			values[index++] = CStringGetTextDatum(maskstr);
		else
			nulls[index++] = true;

		/* auth_method */
		values[index++] = CStringGetTextDatum(hba_authname(hba->auth_method));

		/* options */
		options = get_hba_options(hba);
		if (options)
			values[index++] = PointerGetDatum(options);
		else
			nulls[index++] = true;
	}
	else
	{
		/* no parsing result, so set relevant fields to nulls */
		memset(&nulls[1], true, (NUM_PG_HBA_FILE_RULES_ATTS - 2) * sizeof(bool));
	}

	/* error */
	if (err_msg)
		values[NUM_PG_HBA_FILE_RULES_ATTS - 1] = CStringGetTextDatum(err_msg);
	else
		nulls[NUM_PG_HBA_FILE_RULES_ATTS - 1] = true;

	tuple = heap_form_tuple(tupdesc, values, nulls);
	tuplestore_puttuple(tuple_store, tuple);
}

/*
 * fill_hba_view
 *		Read the pg_hba.conf file and fill the tuplestore with view records.
 */
static void
fill_hba_view(Tuplestorestate *tuple_store, TupleDesc tupdesc)
{
	FILE	   *file;
	List	   *hba_lines = NIL;
	ListCell   *line;
	MemoryContext linecxt;
	MemoryContext hbacxt;
	MemoryContext oldcxt;

	/*
	 * In the unlikely event that we can't open pg_hba.conf, we throw an
	 * error, rather than trying to report it via some sort of view entry.
	 * (Most other error conditions should result in a message in a view
	 * entry.)
	 */
	file = AllocateFile(HbaFileName, "r");
	if (file == NULL)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open configuration file \"%s\": %m",
						HbaFileName)));

	linecxt = tokenize_auth_file(HbaFileName, file, &hba_lines, DEBUG3);
	FreeFile(file);

	/* Now parse all the lines */
	hbacxt = AllocSetContextCreate(CurrentMemoryContext,
								   "hba parser context",
								   ALLOCSET_SMALL_SIZES);
	oldcxt = MemoryContextSwitchTo(hbacxt);
	foreach(line, hba_lines)
	{
		TokenizedAuthLine *tok_line = (TokenizedAuthLine *) lfirst(line);
		HbaLine    *hbaline = NULL;

		/* don't parse lines that already have errors */
		if (tok_line->err_msg == NULL)
			hbaline = parse_hba_line(tok_line, DEBUG3);

		fill_hba_line(tuple_store, tupdesc, tok_line->line_num,
					  hbaline, tok_line->err_msg);
	}

	/* Free tokenizer memory */
	MemoryContextDelete(linecxt);
	/* Free parse_hba_line memory */
	MemoryContextSwitchTo(oldcxt);
	MemoryContextDelete(hbacxt);
}

/*
 * pg_hba_file_rules
 *
 * SQL-accessible set-returning function to return all the entries in the
 * pg_hba.conf file.
 */
Datum
pg_hba_file_rules(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsi;

	/*
	 * Build tuplestore to hold the result rows.  We must use the Materialize
	 * mode to be safe against HBA file changes while the cursor is open. It's
	 * also more efficient than having to look up our current position in the
	 * parsed list every time.
	 */
	SetSingleFuncCall(fcinfo, 0);

	/* Fill the tuplestore */
	rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	fill_hba_view(rsi->setResult, rsi->setDesc);

	PG_RETURN_NULL();
}

/* Number of columns in pg_ident_file_mappings view */
#define NUM_PG_IDENT_FILE_MAPPINGS_ATTS	 5

/*
 * fill_ident_line: build one row of pg_ident_file_mappings view, add it to
 * tuplestore
 *
 * tuple_store: where to store data
 * tupdesc: tuple descriptor for the view
 * lineno: pg_ident.conf line number (must always be valid)
 * ident: parsed line data (can be NULL, in which case err_msg should be set)
 * err_msg: error message (NULL if none)
 *
 * Note: leaks memory, but we don't care since this is run in a short-lived
 * memory context.
 */
static void
fill_ident_line(Tuplestorestate *tuple_store, TupleDesc tupdesc,
				int lineno, IdentLine *ident, const char *err_msg)
{
	Datum		values[NUM_PG_IDENT_FILE_MAPPINGS_ATTS];
	bool		nulls[NUM_PG_IDENT_FILE_MAPPINGS_ATTS];
	HeapTuple	tuple;
	int			index;

	Assert(tupdesc->natts == NUM_PG_IDENT_FILE_MAPPINGS_ATTS);

	memset(values, 0, sizeof(values));
	memset(nulls, 0, sizeof(nulls));
	index = 0;

	/* line_number */
	values[index++] = Int32GetDatum(lineno);

	if (ident != NULL)
	{
		values[index++] = CStringGetTextDatum(ident->usermap);
		values[index++] = CStringGetTextDatum(ident->ident_user);
		values[index++] = CStringGetTextDatum(ident->pg_role);
	}
	else
	{
		/* no parsing result, so set relevant fields to nulls */
		memset(&nulls[1], true, (NUM_PG_IDENT_FILE_MAPPINGS_ATTS - 2) * sizeof(bool));
	}

	/* error */
	if (err_msg)
		values[NUM_PG_IDENT_FILE_MAPPINGS_ATTS - 1] = CStringGetTextDatum(err_msg);
	else
		nulls[NUM_PG_IDENT_FILE_MAPPINGS_ATTS - 1] = true;

	tuple = heap_form_tuple(tupdesc, values, nulls);
	tuplestore_puttuple(tuple_store, tuple);
}

/*
 * Read the pg_ident.conf file and fill the tuplestore with view records.
 */
static void
fill_ident_view(Tuplestorestate *tuple_store, TupleDesc tupdesc)
{
	FILE	   *file;
	List	   *ident_lines = NIL;
	ListCell   *line;
	MemoryContext linecxt;
	MemoryContext identcxt;
	MemoryContext oldcxt;

	/*
	 * In the unlikely event that we can't open pg_ident.conf, we throw an
	 * error, rather than trying to report it via some sort of view entry.
	 * (Most other error conditions should result in a message in a view
	 * entry.)
	 */
	file = AllocateFile(IdentFileName, "r");
	if (file == NULL)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open usermap file \"%s\": %m",
						IdentFileName)));

	linecxt = tokenize_auth_file(IdentFileName, file, &ident_lines, DEBUG3);
	FreeFile(file);

	/* Now parse all the lines */
	identcxt = AllocSetContextCreate(CurrentMemoryContext,
									 "ident parser context",
									 ALLOCSET_SMALL_SIZES);
	oldcxt = MemoryContextSwitchTo(identcxt);
	foreach(line, ident_lines)
	{
		TokenizedAuthLine *tok_line = (TokenizedAuthLine *) lfirst(line);
		IdentLine  *identline = NULL;

		/* don't parse lines that already have errors */
		if (tok_line->err_msg == NULL)
			identline = parse_ident_line(tok_line, DEBUG3);

		fill_ident_line(tuple_store, tupdesc, tok_line->line_num, identline,
						tok_line->err_msg);
	}

	/* Free tokenizer memory */
	MemoryContextDelete(linecxt);
	/* Free parse_ident_line memory */
	MemoryContextSwitchTo(oldcxt);
	MemoryContextDelete(identcxt);
}

/*
 * SQL-accessible SRF to return all the entries in the pg_ident.conf file.
 */
Datum
pg_ident_file_mappings(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsi;

	/*
	 * Build tuplestore to hold the result rows.  We must use the Materialize
	 * mode to be safe against HBA file changes while the cursor is open. It's
	 * also more efficient than having to look up our current position in the
	 * parsed list every time.
	 */
	SetSingleFuncCall(fcinfo, 0);

	/* Fill the tuplestore */
	rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	fill_ident_view(rsi->setResult, rsi->setDesc);

	PG_RETURN_NULL();
}
