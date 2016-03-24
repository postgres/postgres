/*-------------------------------------------------------------------------
 *
 * Utility routines for SQL dumping
 *
 * Basically this is stuff that is useful in both pg_dump and pg_dumpall.
 *
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/bin/pg_dump/dumputils.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include "dumputils.h"
#include "fe_utils/string_utils.h"


#define supports_grant_options(version) ((version) >= 70400)

static bool parseAclItem(const char *item, const char *type,
			 const char *name, const char *subname, int remoteVersion,
			 PQExpBuffer grantee, PQExpBuffer grantor,
			 PQExpBuffer privs, PQExpBuffer privswgo);
static char *copyAclUserName(PQExpBuffer output, char *input);
static void AddAcl(PQExpBuffer aclbuf, const char *keyword,
	   const char *subname);


/*
 * Build GRANT/REVOKE command(s) for an object.
 *
 *	name: the object name, in the form to use in the commands (already quoted)
 *	subname: the sub-object name, if any (already quoted); NULL if none
 *	type: the object type (as seen in GRANT command: must be one of
 *		TABLE, SEQUENCE, FUNCTION, LANGUAGE, SCHEMA, DATABASE, TABLESPACE,
 *		FOREIGN DATA WRAPPER, SERVER, or LARGE OBJECT)
 *	acls: the ACL string fetched from the database
 *	owner: username of object owner (will be passed through fmtId); can be
 *		NULL or empty string to indicate "no owner known"
 *	prefix: string to prefix to each generated command; typically empty
 *	remoteVersion: version of database
 *
 * Returns TRUE if okay, FALSE if could not parse the acl string.
 * The resulting commands (if any) are appended to the contents of 'sql'.
 *
 * Note: when processing a default ACL, prefix is "ALTER DEFAULT PRIVILEGES "
 * or something similar, and name is an empty string.
 *
 * Note: beware of passing a fmtId() result directly as 'name' or 'subname',
 * since this routine uses fmtId() internally.
 */
bool
buildACLCommands(const char *name, const char *subname,
				 const char *type, const char *acls, const char *owner,
				 const char *prefix, int remoteVersion,
				 PQExpBuffer sql)
{
	bool		ok = true;
	char	  **aclitems;
	int			naclitems;
	int			i;
	PQExpBuffer grantee,
				grantor,
				privs,
				privswgo;
	PQExpBuffer firstsql,
				secondsql;
	bool		found_owner_privs = false;

	if (strlen(acls) == 0)
		return true;			/* object has default permissions */

	/* treat empty-string owner same as NULL */
	if (owner && *owner == '\0')
		owner = NULL;

	if (!parsePGArray(acls, &aclitems, &naclitems))
	{
		if (aclitems)
			free(aclitems);
		return false;
	}

	grantee = createPQExpBuffer();
	grantor = createPQExpBuffer();
	privs = createPQExpBuffer();
	privswgo = createPQExpBuffer();

	/*
	 * At the end, these two will be pasted together to form the result. But
	 * the owner privileges need to go before the other ones to keep the
	 * dependencies valid.  In recent versions this is normally the case, but
	 * in old versions they come after the PUBLIC privileges and that results
	 * in problems if we need to run REVOKE on the owner privileges.
	 */
	firstsql = createPQExpBuffer();
	secondsql = createPQExpBuffer();

	/*
	 * Always start with REVOKE ALL FROM PUBLIC, so that we don't have to
	 * wire-in knowledge about the default public privileges for different
	 * kinds of objects.
	 */
	appendPQExpBuffer(firstsql, "%sREVOKE ALL", prefix);
	if (subname)
		appendPQExpBuffer(firstsql, "(%s)", subname);
	appendPQExpBuffer(firstsql, " ON %s %s FROM PUBLIC;\n", type, name);

	/*
	 * We still need some hacking though to cover the case where new default
	 * public privileges are added in new versions: the REVOKE ALL will revoke
	 * them, leading to behavior different from what the old version had,
	 * which is generally not what's wanted.  So add back default privs if the
	 * source database is too old to have had that particular priv.
	 */
	if (remoteVersion < 80200 && strcmp(type, "DATABASE") == 0)
	{
		/* database CONNECT priv didn't exist before 8.2 */
		appendPQExpBuffer(firstsql, "%sGRANT CONNECT ON %s %s TO PUBLIC;\n",
						  prefix, type, name);
	}

	/* Scan individual ACL items */
	for (i = 0; i < naclitems; i++)
	{
		if (!parseAclItem(aclitems[i], type, name, subname, remoteVersion,
						  grantee, grantor, privs, privswgo))
		{
			ok = false;
			break;
		}

		if (grantor->len == 0 && owner)
			printfPQExpBuffer(grantor, "%s", owner);

		if (privs->len > 0 || privswgo->len > 0)
		{
			if (owner
				&& strcmp(grantee->data, owner) == 0
				&& strcmp(grantor->data, owner) == 0)
			{
				found_owner_privs = true;

				/*
				 * For the owner, the default privilege level is ALL WITH
				 * GRANT OPTION (only ALL prior to 7.4).
				 */
				if (supports_grant_options(remoteVersion)
					? strcmp(privswgo->data, "ALL") != 0
					: strcmp(privs->data, "ALL") != 0)
				{
					appendPQExpBuffer(firstsql, "%sREVOKE ALL", prefix);
					if (subname)
						appendPQExpBuffer(firstsql, "(%s)", subname);
					appendPQExpBuffer(firstsql, " ON %s %s FROM %s;\n",
									  type, name, fmtId(grantee->data));
					if (privs->len > 0)
						appendPQExpBuffer(firstsql,
										  "%sGRANT %s ON %s %s TO %s;\n",
										  prefix, privs->data, type, name,
										  fmtId(grantee->data));
					if (privswgo->len > 0)
						appendPQExpBuffer(firstsql,
							"%sGRANT %s ON %s %s TO %s WITH GRANT OPTION;\n",
										  prefix, privswgo->data, type, name,
										  fmtId(grantee->data));
				}
			}
			else
			{
				/*
				 * Otherwise can assume we are starting from no privs.
				 */
				if (grantor->len > 0
					&& (!owner || strcmp(owner, grantor->data) != 0))
					appendPQExpBuffer(secondsql, "SET SESSION AUTHORIZATION %s;\n",
									  fmtId(grantor->data));

				if (privs->len > 0)
				{
					appendPQExpBuffer(secondsql, "%sGRANT %s ON %s %s TO ",
									  prefix, privs->data, type, name);
					if (grantee->len == 0)
						appendPQExpBufferStr(secondsql, "PUBLIC;\n");
					else if (strncmp(grantee->data, "group ",
									 strlen("group ")) == 0)
						appendPQExpBuffer(secondsql, "GROUP %s;\n",
									fmtId(grantee->data + strlen("group ")));
					else
						appendPQExpBuffer(secondsql, "%s;\n", fmtId(grantee->data));
				}
				if (privswgo->len > 0)
				{
					appendPQExpBuffer(secondsql, "%sGRANT %s ON %s %s TO ",
									  prefix, privswgo->data, type, name);
					if (grantee->len == 0)
						appendPQExpBufferStr(secondsql, "PUBLIC");
					else if (strncmp(grantee->data, "group ",
									 strlen("group ")) == 0)
						appendPQExpBuffer(secondsql, "GROUP %s",
									fmtId(grantee->data + strlen("group ")));
					else
						appendPQExpBufferStr(secondsql, fmtId(grantee->data));
					appendPQExpBufferStr(secondsql, " WITH GRANT OPTION;\n");
				}

				if (grantor->len > 0
					&& (!owner || strcmp(owner, grantor->data) != 0))
					appendPQExpBufferStr(secondsql, "RESET SESSION AUTHORIZATION;\n");
			}
		}
	}

	/*
	 * If we didn't find any owner privs, the owner must have revoked 'em all
	 */
	if (!found_owner_privs && owner)
	{
		appendPQExpBuffer(firstsql, "%sREVOKE ALL", prefix);
		if (subname)
			appendPQExpBuffer(firstsql, "(%s)", subname);
		appendPQExpBuffer(firstsql, " ON %s %s FROM %s;\n",
						  type, name, fmtId(owner));
	}

	destroyPQExpBuffer(grantee);
	destroyPQExpBuffer(grantor);
	destroyPQExpBuffer(privs);
	destroyPQExpBuffer(privswgo);

	appendPQExpBuffer(sql, "%s%s", firstsql->data, secondsql->data);
	destroyPQExpBuffer(firstsql);
	destroyPQExpBuffer(secondsql);

	free(aclitems);

	return ok;
}

/*
 * Build ALTER DEFAULT PRIVILEGES command(s) for single pg_default_acl entry.
 *
 *	type: the object type (TABLES, FUNCTIONS, etc)
 *	nspname: schema name, or NULL for global default privileges
 *	acls: the ACL string fetched from the database
 *	owner: username of privileges owner (will be passed through fmtId)
 *	remoteVersion: version of database
 *
 * Returns TRUE if okay, FALSE if could not parse the acl string.
 * The resulting commands (if any) are appended to the contents of 'sql'.
 */
bool
buildDefaultACLCommands(const char *type, const char *nspname,
						const char *acls, const char *owner,
						int remoteVersion,
						PQExpBuffer sql)
{
	bool		result;
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

	result = buildACLCommands("", NULL,
							  type, acls, owner,
							  prefix->data, remoteVersion,
							  sql);

	destroyPQExpBuffer(prefix);

	return result;
}

/*
 * This will parse an aclitem string, having the general form
 *		username=privilegecodes/grantor
 * or
 *		group groupname=privilegecodes/grantor
 * (the /grantor part will not be present if pre-7.4 database).
 *
 * The returned grantee string will be the dequoted username or groupname
 * (preceded with "group " in the latter case).  The returned grantor is
 * the dequoted grantor name or empty.  Privilege characters are decoded
 * and split between privileges with grant option (privswgo) and without
 * (privs).
 *
 * Note: for cross-version compatibility, it's important to use ALL when
 * appropriate.
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

	buf = strdup(item);
	if (!buf)
		return false;

	/* user or group name is string up to = */
	eqpos = copyAclUserName(grantee, buf);
	if (*eqpos != '=')
	{
		free(buf);
		return false;
	}

	/* grantor may be listed after / */
	slpos = strchr(eqpos + 1, '/');
	if (slpos)
	{
		*slpos++ = '\0';
		slpos = copyAclUserName(grantor, slpos);
		if (*slpos != '\0')
		{
			free(buf);
			return false;
		}
	}
	else
		resetPQExpBuffer(grantor);

	/* privilege codes */
#define CONVERT_PRIV(code, keywd) \
do { \
	if ((pos = strchr(eqpos + 1, code))) \
	{ \
		if (*(pos + 1) == '*') \
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
			if (remoteVersion >= 70200)
				CONVERT_PRIV('x', "REFERENCES");
			/* rest are not applicable to columns */
			if (subname == NULL)
			{
				if (remoteVersion >= 70200)
				{
					CONVERT_PRIV('d', "DELETE");
					CONVERT_PRIV('t', "TRIGGER");
				}
				if (remoteVersion >= 80400)
					CONVERT_PRIV('D', "TRUNCATE");
			}
		}

		/* UPDATE */
		if (remoteVersion >= 70200 ||
			strcmp(type, "SEQUENCE") == 0 ||
			strcmp(type, "SEQUENCES") == 0)
			CONVERT_PRIV('w', "UPDATE");
		else
			/* 7.0 and 7.1 have a simpler worldview */
			CONVERT_PRIV('w', "UPDATE,DELETE");
	}
	else if (strcmp(type, "FUNCTION") == 0 ||
			 strcmp(type, "FUNCTIONS") == 0)
		CONVERT_PRIV('X', "EXECUTE");
	else if (strcmp(type, "LANGUAGE") == 0)
		CONVERT_PRIV('U', "USAGE");
	else if (strcmp(type, "SCHEMA") == 0)
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

	free(buf);

	return true;
}

/*
 * Transfer a user or group name starting at *input into the output buffer,
 * dequoting if needed.  Returns a pointer to just past the input name.
 * The name is taken to end at an unquoted '=' or end of string.
 */
static char *
copyAclUserName(PQExpBuffer output, char *input)
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
					return input;		/* really a syntax error... */

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
 */
void
buildShSecLabelQuery(PGconn *conn, const char *catalog_name, uint32 objectId,
					 PQExpBuffer sql)
{
	appendPQExpBuffer(sql,
					  "SELECT provider, label FROM pg_catalog.pg_shseclabel "
					  "WHERE classoid = '%s'::pg_catalog.regclass AND "
					  "objoid = %u", catalog_name, objectId);
}

/*
 * emitShSecLabels
 *
 * Format security label data retrieved by the query generated in
 * buildShSecLabelQuery.
 */
void
emitShSecLabels(PGconn *conn, PGresult *res, PQExpBuffer buffer,
				const char *target, const char *objname)
{
	int			i;

	for (i = 0; i < PQntuples(res); i++)
	{
		char	   *provider = PQgetvalue(res, i, 0);
		char	   *label = PQgetvalue(res, i, 1);

		/* must use fmtId result before calling it again */
		appendPQExpBuffer(buffer,
						  "SECURITY LABEL FOR %s ON %s",
						  fmtId(provider), target);
		appendPQExpBuffer(buffer,
						  " %s IS ",
						  fmtId(objname));
		appendStringLiteralConn(buffer, label, conn);
		appendPQExpBufferStr(buffer, ";\n");
	}
}
