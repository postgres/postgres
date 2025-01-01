/*-------------------------------------------------------------------------
 *
 * Utility routines for SQL dumping
 *
 * Basically this is stuff that is useful in both pg_dump and pg_dumpall.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/bin/pg_dump/dumputils.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include <ctype.h>

#include "dumputils.h"
#include "fe_utils/string_utils.h"


static bool parseAclItem(const char *item, const char *type,
						 const char *name, const char *subname, int remoteVersion,
						 PQExpBuffer grantee, PQExpBuffer grantor,
						 PQExpBuffer privs, PQExpBuffer privswgo);
static char *dequoteAclUserName(PQExpBuffer output, char *input);
static void AddAcl(PQExpBuffer aclbuf, const char *keyword,
				   const char *subname);


/*
 * Build GRANT/REVOKE command(s) for an object.
 *
 *	name: the object name, in the form to use in the commands (already quoted)
 *	subname: the sub-object name, if any (already quoted); NULL if none
 *	nspname: the namespace the object is in (NULL if none); not pre-quoted
 *	type: the object type (as seen in GRANT command: must be one of
 *		TABLE, SEQUENCE, FUNCTION, PROCEDURE, LANGUAGE, SCHEMA, DATABASE, TABLESPACE,
 *		FOREIGN DATA WRAPPER, SERVER, PARAMETER or LARGE OBJECT)
 *	acls: the ACL string fetched from the database
 *	baseacls: the initial ACL string for this object
 *	owner: username of object owner (will be passed through fmtId); can be
 *		NULL or empty string to indicate "no owner known"
 *	prefix: string to prefix to each generated command; typically empty
 *	remoteVersion: version of database
 *
 * Returns true if okay, false if could not parse the acl string.
 * The resulting commands (if any) are appended to the contents of 'sql'.
 *
 * baseacls is typically the result of acldefault() for the object's type
 * and owner.  However, if there is a pg_init_privs entry for the object,
 * it should instead be the initprivs ACLs.  When acls is itself a
 * pg_init_privs entry, baseacls is what to dump that relative to; then
 * it can be either an acldefault() value or an empty ACL "{}".
 *
 * Note: when processing a default ACL, prefix is "ALTER DEFAULT PRIVILEGES "
 * or something similar, and name is an empty string.
 *
 * Note: beware of passing a fmtId() result directly as 'name' or 'subname',
 * since this routine uses fmtId() internally.
 */
bool
buildACLCommands(const char *name, const char *subname, const char *nspname,
				 const char *type, const char *acls, const char *baseacls,
				 const char *owner, const char *prefix, int remoteVersion,
				 PQExpBuffer sql)
{
	bool		ok = true;
	char	  **aclitems = NULL;
	char	  **baseitems = NULL;
	char	  **grantitems = NULL;
	char	  **revokeitems = NULL;
	int			naclitems = 0;
	int			nbaseitems = 0;
	int			ngrantitems = 0;
	int			nrevokeitems = 0;
	int			i;
	PQExpBuffer grantee,
				grantor,
				privs,
				privswgo;
	PQExpBuffer firstsql,
				secondsql;

	/*
	 * If the acl was NULL (initial default state), we need do nothing.  Note
	 * that this is distinguishable from all-privileges-revoked, which will
	 * look like an empty array ("{}").
	 */
	if (acls == NULL || *acls == '\0')
		return true;			/* object has default permissions */

	/* treat empty-string owner same as NULL */
	if (owner && *owner == '\0')
		owner = NULL;

	/* Parse the acls array */
	if (!parsePGArray(acls, &aclitems, &naclitems))
	{
		free(aclitems);
		return false;
	}

	/* Parse the baseacls too */
	if (!parsePGArray(baseacls, &baseitems, &nbaseitems))
	{
		free(aclitems);
		free(baseitems);
		return false;
	}

	/*
	 * Compare the actual ACL with the base ACL, extracting the privileges
	 * that need to be granted (i.e., are in the actual ACL but not the base
	 * ACL) and the ones that need to be revoked (the reverse).  We use plain
	 * string comparisons to check for matches.  In principle that could be
	 * fooled by extraneous issues such as whitespace, but since all these
	 * strings are the work of aclitemout(), it should be OK in practice.
	 * Besides, a false mismatch will just cause the output to be a little
	 * more verbose than it really needed to be.
	 */
	grantitems = (char **) pg_malloc(naclitems * sizeof(char *));
	for (i = 0; i < naclitems; i++)
	{
		bool		found = false;

		for (int j = 0; j < nbaseitems; j++)
		{
			if (strcmp(aclitems[i], baseitems[j]) == 0)
			{
				found = true;
				break;
			}
		}
		if (!found)
			grantitems[ngrantitems++] = aclitems[i];
	}
	revokeitems = (char **) pg_malloc(nbaseitems * sizeof(char *));
	for (i = 0; i < nbaseitems; i++)
	{
		bool		found = false;

		for (int j = 0; j < naclitems; j++)
		{
			if (strcmp(baseitems[i], aclitems[j]) == 0)
			{
				found = true;
				break;
			}
		}
		if (!found)
			revokeitems[nrevokeitems++] = baseitems[i];
	}

	/* Prepare working buffers */
	grantee = createPQExpBuffer();
	grantor = createPQExpBuffer();
	privs = createPQExpBuffer();
	privswgo = createPQExpBuffer();

	/*
	 * At the end, these two will be pasted together to form the result.
	 */
	firstsql = createPQExpBuffer();
	secondsql = createPQExpBuffer();

	/*
	 * Build REVOKE statements for ACLs listed in revokeitems[].
	 */
	for (i = 0; i < nrevokeitems; i++)
	{
		if (!parseAclItem(revokeitems[i],
						  type, name, subname, remoteVersion,
						  grantee, grantor, privs, NULL))
		{
			ok = false;
			break;
		}

		if (privs->len > 0)
		{
			appendPQExpBuffer(firstsql, "%sREVOKE %s ON %s ",
							  prefix, privs->data, type);
			if (nspname && *nspname)
				appendPQExpBuffer(firstsql, "%s.", fmtId(nspname));
			if (name && *name)
				appendPQExpBuffer(firstsql, "%s ", name);
			appendPQExpBufferStr(firstsql, "FROM ");
			if (grantee->len == 0)
				appendPQExpBufferStr(firstsql, "PUBLIC;\n");
			else
				appendPQExpBuffer(firstsql, "%s;\n",
								  fmtId(grantee->data));
		}
	}

	/*
	 * At this point we have issued REVOKE statements for all initial and
	 * default privileges that are no longer present on the object, so we are
	 * almost ready to GRANT the privileges listed in grantitems[].
	 *
	 * We still need some hacking though to cover the case where new default
	 * public privileges are added in new versions: the REVOKE ALL will revoke
	 * them, leading to behavior different from what the old version had,
	 * which is generally not what's wanted.  So add back default privs if the
	 * source database is too old to have had that particular priv.  (As of
	 * right now, no such cases exist in supported versions.)
	 */

	/*
	 * Scan individual ACL items to be granted.
	 *
	 * The order in which privileges appear in the ACL string (the order they
	 * have been GRANT'd in, which the backend maintains) must be preserved to
	 * ensure that GRANTs WITH GRANT OPTION and subsequent GRANTs based on
	 * those are dumped in the correct order.  However, some old server
	 * versions will show grants to PUBLIC before the owner's own grants; for
	 * consistency's sake, force the owner's grants to be output first.
	 */
	for (i = 0; i < ngrantitems; i++)
	{
		if (parseAclItem(grantitems[i], type, name, subname, remoteVersion,
						 grantee, grantor, privs, privswgo))
		{
			/*
			 * If the grantor isn't the owner, we'll need to use SET SESSION
			 * AUTHORIZATION to become the grantor.  Issue the SET/RESET only
			 * if there's something useful to do.
			 */
			if (privs->len > 0 || privswgo->len > 0)
			{
				PQExpBuffer thissql;

				/* Set owner as grantor if that's not explicit in the ACL */
				if (grantor->len == 0 && owner)
					printfPQExpBuffer(grantor, "%s", owner);

				/* Make sure owner's own grants are output before others */
				if (owner &&
					strcmp(grantee->data, owner) == 0 &&
					strcmp(grantor->data, owner) == 0)
					thissql = firstsql;
				else
					thissql = secondsql;

				if (grantor->len > 0
					&& (!owner || strcmp(owner, grantor->data) != 0))
					appendPQExpBuffer(thissql, "SET SESSION AUTHORIZATION %s;\n",
									  fmtId(grantor->data));

				if (privs->len > 0)
				{
					appendPQExpBuffer(thissql, "%sGRANT %s ON %s ",
									  prefix, privs->data, type);
					if (nspname && *nspname)
						appendPQExpBuffer(thissql, "%s.", fmtId(nspname));
					if (name && *name)
						appendPQExpBuffer(thissql, "%s ", name);
					appendPQExpBufferStr(thissql, "TO ");
					if (grantee->len == 0)
						appendPQExpBufferStr(thissql, "PUBLIC;\n");
					else
						appendPQExpBuffer(thissql, "%s;\n", fmtId(grantee->data));
				}
				if (privswgo->len > 0)
				{
					appendPQExpBuffer(thissql, "%sGRANT %s ON %s ",
									  prefix, privswgo->data, type);
					if (nspname && *nspname)
						appendPQExpBuffer(thissql, "%s.", fmtId(nspname));
					if (name && *name)
						appendPQExpBuffer(thissql, "%s ", name);
					appendPQExpBufferStr(thissql, "TO ");
					if (grantee->len == 0)
						appendPQExpBufferStr(thissql, "PUBLIC");
					else
						appendPQExpBufferStr(thissql, fmtId(grantee->data));
					appendPQExpBufferStr(thissql, " WITH GRANT OPTION;\n");
				}

				if (grantor->len > 0
					&& (!owner || strcmp(owner, grantor->data) != 0))
					appendPQExpBufferStr(thissql, "RESET SESSION AUTHORIZATION;\n");
			}
		}
		else
		{
			/* parseAclItem failed, give up */
			ok = false;
			break;
		}
	}

	destroyPQExpBuffer(grantee);
	destroyPQExpBuffer(grantor);
	destroyPQExpBuffer(privs);
	destroyPQExpBuffer(privswgo);

	appendPQExpBuffer(sql, "%s%s", firstsql->data, secondsql->data);
	destroyPQExpBuffer(firstsql);
	destroyPQExpBuffer(secondsql);

	free(aclitems);
	free(baseitems);
	free(grantitems);
	free(revokeitems);

	return ok;
}

/*
 * Build ALTER DEFAULT PRIVILEGES command(s) for a single pg_default_acl entry.
 *
 *	type: the object type (TABLES, FUNCTIONS, etc)
 *	nspname: schema name, or NULL for global default privileges
 *	acls: the ACL string fetched from the database
 *	acldefault: the appropriate default ACL for the object type and owner
 *	owner: username of privileges owner (will be passed through fmtId)
 *	remoteVersion: version of database
 *
 * Returns true if okay, false if could not parse the acl string.
 * The resulting commands (if any) are appended to the contents of 'sql'.
 */
bool
buildDefaultACLCommands(const char *type, const char *nspname,
						const char *acls, const char *acldefault,
						const char *owner,
						int remoteVersion,
						PQExpBuffer sql)
{
	PQExpBuffer prefix;

	prefix = createPQExpBuffer();

	/*
	 * We incorporate the target role directly into the command, rather than
	 * playing around with SET ROLE or anything like that.  This is so that a
	 * permissions error leads to nothing happening, rather than changing
	 * default privileges for the wrong user.
	 */
	appendPQExpBuffer(prefix, "ALTER DEFAULT PRIVILEGES FOR ROLE %s ",
					  fmtId(owner));
	if (nspname)
		appendPQExpBuffer(prefix, "IN SCHEMA %s ", fmtId(nspname));

	/*
	 * There's no such thing as initprivs for a default ACL, so the base ACL
	 * is always just the object-type-specific default.
	 */
	if (!buildACLCommands("", NULL, NULL, type,
						  acls, acldefault, owner,
						  prefix->data, remoteVersion, sql))
	{
		destroyPQExpBuffer(prefix);
		return false;
	}

	destroyPQExpBuffer(prefix);

	return true;
}

/*
 * This will parse an aclitem string, having the general form
 *		username=privilegecodes/grantor
 *
 * Returns true on success, false on parse error.  On success, the components
 * of the string are returned in the PQExpBuffer parameters.
 *
 * The returned grantee string will be the dequoted username, or an empty
 * string in the case of a grant to PUBLIC.  The returned grantor is the
 * dequoted grantor name.  Privilege characters are translated to GRANT/REVOKE
 * comma-separated privileges lists.  If "privswgo" is non-NULL, the result is
 * separate lists for privileges with grant option ("privswgo") and without
 * ("privs").  Otherwise, "privs" bears every relevant privilege, ignoring the
 * grant option distinction.
 *
 * Note: for cross-version compatibility, it's important to use ALL to
 * represent the privilege sets whenever appropriate.
 */
static bool
parseAclItem(const char *item, const char *type,
			 const char *name, const char *subname, int remoteVersion,
			 PQExpBuffer grantee, PQExpBuffer grantor,
			 PQExpBuffer privs, PQExpBuffer privswgo)
{
	char	   *buf;
	bool		all_with_go = true;
	bool		all_without_go = true;
	char	   *eqpos;
	char	   *slpos;
	char	   *pos;

	buf = pg_strdup(item);

	/* user or group name is string up to = */
	eqpos = dequoteAclUserName(grantee, buf);
	if (*eqpos != '=')
	{
		pg_free(buf);
		return false;
	}

	/* grantor should appear after / */
	slpos = strchr(eqpos + 1, '/');
	if (slpos)
	{
		*slpos++ = '\0';
		slpos = dequoteAclUserName(grantor, slpos);
		if (*slpos != '\0')
		{
			pg_free(buf);
			return false;
		}
	}
	else
	{
		pg_free(buf);
		return false;
	}

	/* privilege codes */
#define CONVERT_PRIV(code, keywd) \
do { \
	if ((pos = strchr(eqpos + 1, code))) \
	{ \
		if (*(pos + 1) == '*' && privswgo != NULL) \
		{ \
			AddAcl(privswgo, keywd, subname); \
			all_without_go = false; \
		} \
		else \
		{ \
			AddAcl(privs, keywd, subname); \
			all_with_go = false; \
		} \
	} \
	else \
		all_with_go = all_without_go = false; \
} while (0)

	resetPQExpBuffer(privs);
	resetPQExpBuffer(privswgo);

	if (strcmp(type, "TABLE") == 0 || strcmp(type, "SEQUENCE") == 0 ||
		strcmp(type, "TABLES") == 0 || strcmp(type, "SEQUENCES") == 0)
	{
		CONVERT_PRIV('r', "SELECT");

		if (strcmp(type, "SEQUENCE") == 0 ||
			strcmp(type, "SEQUENCES") == 0)
			/* sequence only */
			CONVERT_PRIV('U', "USAGE");
		else
		{
			/* table only */
			CONVERT_PRIV('a', "INSERT");
			CONVERT_PRIV('x', "REFERENCES");
			/* rest are not applicable to columns */
			if (subname == NULL)
			{
				CONVERT_PRIV('d', "DELETE");
				CONVERT_PRIV('t', "TRIGGER");
				CONVERT_PRIV('D', "TRUNCATE");
				CONVERT_PRIV('m', "MAINTAIN");
			}
		}

		/* UPDATE */
		CONVERT_PRIV('w', "UPDATE");
	}
	else if (strcmp(type, "FUNCTION") == 0 ||
			 strcmp(type, "FUNCTIONS") == 0)
		CONVERT_PRIV('X', "EXECUTE");
	else if (strcmp(type, "PROCEDURE") == 0 ||
			 strcmp(type, "PROCEDURES") == 0)
		CONVERT_PRIV('X', "EXECUTE");
	else if (strcmp(type, "LANGUAGE") == 0)
		CONVERT_PRIV('U', "USAGE");
	else if (strcmp(type, "SCHEMA") == 0 ||
			 strcmp(type, "SCHEMAS") == 0)
	{
		CONVERT_PRIV('C', "CREATE");
		CONVERT_PRIV('U', "USAGE");
	}
	else if (strcmp(type, "DATABASE") == 0)
	{
		CONVERT_PRIV('C', "CREATE");
		CONVERT_PRIV('c', "CONNECT");
		CONVERT_PRIV('T', "TEMPORARY");
	}
	else if (strcmp(type, "TABLESPACE") == 0)
		CONVERT_PRIV('C', "CREATE");
	else if (strcmp(type, "TYPE") == 0 ||
			 strcmp(type, "TYPES") == 0)
		CONVERT_PRIV('U', "USAGE");
	else if (strcmp(type, "FOREIGN DATA WRAPPER") == 0)
		CONVERT_PRIV('U', "USAGE");
	else if (strcmp(type, "FOREIGN SERVER") == 0)
		CONVERT_PRIV('U', "USAGE");
	else if (strcmp(type, "FOREIGN TABLE") == 0)
		CONVERT_PRIV('r', "SELECT");
	else if (strcmp(type, "PARAMETER") == 0)
	{
		CONVERT_PRIV('s', "SET");
		CONVERT_PRIV('A', "ALTER SYSTEM");
	}
	else if (strcmp(type, "LARGE OBJECT") == 0)
	{
		CONVERT_PRIV('r', "SELECT");
		CONVERT_PRIV('w', "UPDATE");
	}
	else
		abort();

#undef CONVERT_PRIV

	if (all_with_go)
	{
		resetPQExpBuffer(privs);
		printfPQExpBuffer(privswgo, "ALL");
		if (subname)
			appendPQExpBuffer(privswgo, "(%s)", subname);
	}
	else if (all_without_go)
	{
		resetPQExpBuffer(privswgo);
		printfPQExpBuffer(privs, "ALL");
		if (subname)
			appendPQExpBuffer(privs, "(%s)", subname);
	}

	pg_free(buf);

	return true;
}

/*
 * Transfer the role name at *input into the output buffer, adding
 * quoting according to the same rules as putid() in backend's acl.c.
 */
void
quoteAclUserName(PQExpBuffer output, const char *input)
{
	const char *src;
	bool		safe = true;

	for (src = input; *src; src++)
	{
		/* This test had better match what putid() does */
		if (!isalnum((unsigned char) *src) && *src != '_')
		{
			safe = false;
			break;
		}
	}
	if (!safe)
		appendPQExpBufferChar(output, '"');
	for (src = input; *src; src++)
	{
		/* A double quote character in a username is encoded as "" */
		if (*src == '"')
			appendPQExpBufferChar(output, '"');
		appendPQExpBufferChar(output, *src);
	}
	if (!safe)
		appendPQExpBufferChar(output, '"');
}

/*
 * Transfer a user or group name starting at *input into the output buffer,
 * dequoting if needed.  Returns a pointer to just past the input name.
 * The name is taken to end at an unquoted '=' or end of string.
 * Note: unlike quoteAclUserName(), this first clears the output buffer.
 */
static char *
dequoteAclUserName(PQExpBuffer output, char *input)
{
	resetPQExpBuffer(output);

	while (*input && *input != '=')
	{
		/*
		 * If user name isn't quoted, then just add it to the output buffer
		 */
		if (*input != '"')
			appendPQExpBufferChar(output, *input++);
		else
		{
			/* Otherwise, it's a quoted username */
			input++;
			/* Loop until we come across an unescaped quote */
			while (!(*input == '"' && *(input + 1) != '"'))
			{
				if (*input == '\0')
					return input;	/* really a syntax error... */

				/*
				 * Quoting convention is to escape " as "".  Keep this code in
				 * sync with putid() in backend's acl.c.
				 */
				if (*input == '"' && *(input + 1) == '"')
					input++;
				appendPQExpBufferChar(output, *input++);
			}
			input++;
		}
	}
	return input;
}

/*
 * Append a privilege keyword to a keyword list, inserting comma if needed.
 */
static void
AddAcl(PQExpBuffer aclbuf, const char *keyword, const char *subname)
{
	if (aclbuf->len > 0)
		appendPQExpBufferChar(aclbuf, ',');
	appendPQExpBufferStr(aclbuf, keyword);
	if (subname)
		appendPQExpBuffer(aclbuf, "(%s)", subname);
}


/*
 * buildShSecLabelQuery
 *
 * Build a query to retrieve security labels for a shared object.
 * The object is identified by its OID plus the name of the catalog
 * it can be found in (e.g., "pg_database" for database names).
 * The query is appended to "sql".  (We don't execute it here so as to
 * keep this file free of assumptions about how to deal with SQL errors.)
 */
void
buildShSecLabelQuery(const char *catalog_name, Oid objectId,
					 PQExpBuffer sql)
{
	appendPQExpBuffer(sql,
					  "SELECT provider, label FROM pg_catalog.pg_shseclabel "
					  "WHERE classoid = 'pg_catalog.%s'::pg_catalog.regclass "
					  "AND objoid = '%u'", catalog_name, objectId);
}

/*
 * emitShSecLabels
 *
 * Construct SECURITY LABEL commands using the data retrieved by the query
 * generated by buildShSecLabelQuery, and append them to "buffer".
 * Here, the target object is identified by its type name (e.g. "DATABASE")
 * and its name (not pre-quoted).
 */
void
emitShSecLabels(PGconn *conn, PGresult *res, PQExpBuffer buffer,
				const char *objtype, const char *objname)
{
	int			i;

	for (i = 0; i < PQntuples(res); i++)
	{
		char	   *provider = PQgetvalue(res, i, 0);
		char	   *label = PQgetvalue(res, i, 1);

		/* must use fmtId result before calling it again */
		appendPQExpBuffer(buffer,
						  "SECURITY LABEL FOR %s ON %s",
						  fmtId(provider), objtype);
		appendPQExpBuffer(buffer,
						  " %s IS ",
						  fmtId(objname));
		appendStringLiteralConn(buffer, label, conn);
		appendPQExpBufferStr(buffer, ";\n");
	}
}


/*
 * Detect whether the given GUC variable is of GUC_LIST_QUOTE type.
 *
 * It'd be better if we could inquire this directly from the backend; but even
 * if there were a function for that, it could only tell us about variables
 * currently known to guc.c, so that it'd be unsafe for extensions to declare
 * GUC_LIST_QUOTE variables anyway.  Lacking a solution for that, it doesn't
 * seem worth the work to do more than have this list, which must be kept in
 * sync with the variables actually marked GUC_LIST_QUOTE in guc_tables.c.
 */
bool
variable_is_guc_list_quote(const char *name)
{
	if (pg_strcasecmp(name, "local_preload_libraries") == 0 ||
		pg_strcasecmp(name, "search_path") == 0 ||
		pg_strcasecmp(name, "session_preload_libraries") == 0 ||
		pg_strcasecmp(name, "shared_preload_libraries") == 0 ||
		pg_strcasecmp(name, "temp_tablespaces") == 0 ||
		pg_strcasecmp(name, "unix_socket_directories") == 0)
		return true;
	else
		return false;
}

/*
 * SplitGUCList --- parse a string containing identifiers or file names
 *
 * This is used to split the value of a GUC_LIST_QUOTE GUC variable, without
 * presuming whether the elements will be taken as identifiers or file names.
 * See comparable code in src/backend/utils/adt/varlena.c.
 *
 * Inputs:
 *	rawstring: the input string; must be overwritable!	On return, it's
 *			   been modified to contain the separated identifiers.
 *	separator: the separator punctuation expected between identifiers
 *			   (typically '.' or ',').  Whitespace may also appear around
 *			   identifiers.
 * Outputs:
 *	namelist: receives a malloc'd, null-terminated array of pointers to
 *			  identifiers within rawstring.  Caller should free this
 *			  even on error return.
 *
 * Returns true if okay, false if there is a syntax error in the string.
 */
bool
SplitGUCList(char *rawstring, char separator,
			 char ***namelist)
{
	char	   *nextp = rawstring;
	bool		done = false;
	char	  **nextptr;

	/*
	 * Since we disallow empty identifiers, this is a conservative
	 * overestimate of the number of pointers we could need.  Allow one for
	 * list terminator.
	 */
	*namelist = nextptr = (char **)
		pg_malloc((strlen(rawstring) / 2 + 2) * sizeof(char *));
	*nextptr = NULL;

	while (isspace((unsigned char) *nextp))
		nextp++;				/* skip leading whitespace */

	if (*nextp == '\0')
		return true;			/* allow empty string */

	/* At the top of the loop, we are at start of a new identifier. */
	do
	{
		char	   *curname;
		char	   *endp;

		if (*nextp == '"')
		{
			/* Quoted name --- collapse quote-quote pairs */
			curname = nextp + 1;
			for (;;)
			{
				endp = strchr(nextp + 1, '"');
				if (endp == NULL)
					return false;	/* mismatched quotes */
				if (endp[1] != '"')
					break;		/* found end of quoted name */
				/* Collapse adjacent quotes into one quote, and look again */
				memmove(endp, endp + 1, strlen(endp));
				nextp = endp;
			}
			/* endp now points at the terminating quote */
			nextp = endp + 1;
		}
		else
		{
			/* Unquoted name --- extends to separator or whitespace */
			curname = nextp;
			while (*nextp && *nextp != separator &&
				   !isspace((unsigned char) *nextp))
				nextp++;
			endp = nextp;
			if (curname == nextp)
				return false;	/* empty unquoted name not allowed */
		}

		while (isspace((unsigned char) *nextp))
			nextp++;			/* skip trailing whitespace */

		if (*nextp == separator)
		{
			nextp++;
			while (isspace((unsigned char) *nextp))
				nextp++;		/* skip leading whitespace for next */
			/* we expect another name, so done remains false */
		}
		else if (*nextp == '\0')
			done = true;
		else
			return false;		/* invalid syntax */

		/* Now safe to overwrite separator with a null */
		*endp = '\0';

		/*
		 * Finished isolating current name --- add it to output array
		 */
		*nextptr++ = curname;

		/* Loop back if we didn't reach end of string */
	} while (!done);

	*nextptr = NULL;
	return true;
}

/*
 * Helper function for dumping "ALTER DATABASE/ROLE SET ..." commands.
 *
 * Parse the contents of configitem (a "name=value" string), wrap it in
 * a complete ALTER command, and append it to buf.
 *
 * type is DATABASE or ROLE, and name is the name of the database or role.
 * If we need an "IN" clause, type2 and name2 similarly define what to put
 * there; otherwise they should be NULL.
 * conn is used only to determine string-literal quoting conventions.
 */
void
makeAlterConfigCommand(PGconn *conn, const char *configitem,
					   const char *type, const char *name,
					   const char *type2, const char *name2,
					   PQExpBuffer buf)
{
	char	   *mine;
	char	   *pos;

	/* Parse the configitem.  If we can't find an "=", silently do nothing. */
	mine = pg_strdup(configitem);
	pos = strchr(mine, '=');
	if (pos == NULL)
	{
		pg_free(mine);
		return;
	}
	*pos++ = '\0';

	/* Build the command, with suitable quoting for everything. */
	appendPQExpBuffer(buf, "ALTER %s %s ", type, fmtId(name));
	if (type2 != NULL && name2 != NULL)
		appendPQExpBuffer(buf, "IN %s %s ", type2, fmtId(name2));
	appendPQExpBuffer(buf, "SET %s TO ", fmtId(mine));

	/*
	 * Variables that are marked GUC_LIST_QUOTE were already fully quoted by
	 * flatten_set_variable_args() before they were put into the setconfig
	 * array.  However, because the quoting rules used there aren't exactly
	 * like SQL's, we have to break the list value apart and then quote the
	 * elements as string literals.  (The elements may be double-quoted as-is,
	 * but we can't just feed them to the SQL parser; it would do the wrong
	 * thing with elements that are zero-length or longer than NAMEDATALEN.)
	 *
	 * Variables that are not so marked should just be emitted as simple
	 * string literals.  If the variable is not known to
	 * variable_is_guc_list_quote(), we'll do that; this makes it unsafe to
	 * use GUC_LIST_QUOTE for extension variables.
	 */
	if (variable_is_guc_list_quote(mine))
	{
		char	  **namelist;
		char	  **nameptr;

		/* Parse string into list of identifiers */
		/* this shouldn't fail really */
		if (SplitGUCList(pos, ',', &namelist))
		{
			for (nameptr = namelist; *nameptr; nameptr++)
			{
				if (nameptr != namelist)
					appendPQExpBufferStr(buf, ", ");
				appendStringLiteralConn(buf, *nameptr, conn);
			}
		}
		pg_free(namelist);
	}
	else
		appendStringLiteralConn(buf, pos, conn);

	appendPQExpBufferStr(buf, ";\n");

	pg_free(mine);
}
