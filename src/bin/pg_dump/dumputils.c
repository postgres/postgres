/*-------------------------------------------------------------------------
 *
 * Utility routines for SQL dumping
 *	Basically this is stuff that is useful in both pg_dump and pg_dumpall.
 *
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Header: /cvsroot/pgsql/src/bin/pg_dump/dumputils.c,v 1.9 2003/08/14 14:19:11 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include "dumputils.h"

#include "parser/keywords.h"


#define supports_grant_options(version) ((version) >= 70400)

static bool parseAclArray(const char *acls, char ***itemarray, int *nitems);
static bool parseAclItem(const char *item, const char *type, const char *name,
			 int remoteVersion,
			 PQExpBuffer grantee, PQExpBuffer grantor,
			 PQExpBuffer privs, PQExpBuffer privswgo);
static char *copyAclUserName(PQExpBuffer output, char *input);
static void AddAcl(PQExpBuffer aclbuf, const char *keyword);


/*
 *	Quotes input string if it's not a legitimate SQL identifier as-is.
 *
 *	Note that the returned string must be used before calling fmtId again,
 *	since we re-use the same return buffer each time.  Non-reentrant but
 *	avoids memory leakage.
 */
const char *
fmtId(const char *rawid)
{
	static PQExpBuffer id_return = NULL;
	const char *cp;
	bool		need_quotes = false;

	if (id_return)				/* first time through? */
		resetPQExpBuffer(id_return);
	else
		id_return = createPQExpBuffer();

	/*
	 * These checks need to match the identifier production in scan.l.
	 * Don't use islower() etc.
	 */

	if (ScanKeywordLookup(rawid))
		need_quotes = true;
	/* slightly different rules for first character */
	else if (!((rawid[0] >= 'a' && rawid[0] <= 'z') || rawid[0] == '_'))
		need_quotes = true;
	else
	{
		/* otherwise check the entire string */
		for (cp = rawid; *cp; cp++)
		{
			if (!((*cp >= 'a' && *cp <= 'z')
				  || (*cp >= '0' && *cp <= '9')
				  || (*cp == '_')))
			{
				need_quotes = true;
				break;
			}
		}
	}

	if (!need_quotes)
	{
		/* no quoting needed */
		appendPQExpBufferStr(id_return, rawid);
	}
	else
	{
		appendPQExpBufferChar(id_return, '\"');
		for (cp = rawid; *cp; cp++)
		{
			/*
			 * Did we find a double-quote in the string? Then make this a
			 * double double-quote per SQL99. Before, we put in a
			 * backslash/double-quote pair. - thomas 2000-08-05
			 */
			if (*cp == '\"')
				appendPQExpBufferChar(id_return, '\"');
			appendPQExpBufferChar(id_return, *cp);
		}
		appendPQExpBufferChar(id_return, '\"');
	}

	return id_return->data;
}


/*
 * Convert a string value to an SQL string literal and append it to
 * the given buffer.
 *
 * Special characters are escaped. Quote mark ' goes to '' per SQL
 * standard, other stuff goes to \ sequences.  If escapeAll is false,
 * whitespace characters are not escaped (tabs, newlines, etc.).  This
 * is appropriate for dump file output.
 */
void
appendStringLiteral(PQExpBuffer buf, const char *str, bool escapeAll)
{
	appendPQExpBufferChar(buf, '\'');
	while (*str)
	{
		char		ch = *str++;

		if (ch == '\\' || ch == '\'')
		{
			appendPQExpBufferChar(buf, ch);		/* double these */
			appendPQExpBufferChar(buf, ch);
		}
		else if ((unsigned char) ch < (unsigned char) ' ' &&
				 (escapeAll
				  || (ch != '\t' && ch != '\n' && ch != '\v' && ch != '\f' && ch != '\r')
				  ))
		{
			/*
			 * generate octal escape for control chars other than
			 * whitespace
			 */
			appendPQExpBufferChar(buf, '\\');
			appendPQExpBufferChar(buf, ((ch >> 6) & 3) + '0');
			appendPQExpBufferChar(buf, ((ch >> 3) & 7) + '0');
			appendPQExpBufferChar(buf, (ch & 7) + '0');
		}
		else
			appendPQExpBufferChar(buf, ch);
	}
	appendPQExpBufferChar(buf, '\'');
}


/*
 * Convert backend's version string into a number.
 */
int
parse_version(const char *versionString)
{
	int			cnt;
	int			vmaj,
				vmin,
				vrev;

	cnt = sscanf(versionString, "%d.%d.%d", &vmaj, &vmin, &vrev);

	if (cnt < 2)
		return -1;

	if (cnt == 2)
		vrev = 0;

	return (100 * vmaj + vmin) * 100 + vrev;
}


/*
 * Build GRANT/REVOKE command(s) for an object.
 *
 *	name: the object name, in the form to use in the commands (already quoted)
 *	type: the object type (as seen in GRANT command: must be one of
 *		TABLE, FUNCTION, LANGUAGE, or SCHEMA, or DATABASE)
 *	acls: the ACL string fetched from the database
 *	owner: username of object owner (will be passed through fmtId), or NULL
 *	remoteVersion: version of database
 *
 * Returns TRUE if okay, FALSE if could not parse the acl string.
 * The resulting commands (if any) are appended to the contents of 'sql'.
 *
 * Note: beware of passing fmtId() result as 'name', since this routine
 * uses fmtId() internally.
 */
bool
buildACLCommands(const char *name, const char *type,
				 const char *acls, const char *owner,
				 int remoteVersion,
				 PQExpBuffer sql)
{
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

	if (!parseAclArray(acls, &aclitems, &naclitems))
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
	 * At the end, these two will be pasted together to form the result.
	 * But the owner privileges need to go before the other ones to keep
	 * the dependencies valid.	In recent versions this is normally the
	 * case, but in old versions they come after the PUBLIC privileges and
	 * that results in problems if we need to run REVOKE on the owner
	 * privileges.
	 */
	firstsql = createPQExpBuffer();
	secondsql = createPQExpBuffer();

	/*
	 * Always start with REVOKE ALL FROM PUBLIC, so that we don't have to
	 * wire-in knowledge about the default public privileges for different
	 * kinds of objects.
	 */
	appendPQExpBuffer(firstsql, "REVOKE ALL ON %s %s FROM PUBLIC;\n",
					  type, name);

	/* Scan individual ACL items */
	for (i = 0; i < naclitems; i++)
	{
		if (!parseAclItem(aclitems[i], type, name, remoteVersion,
						  grantee, grantor, privs, privswgo))
			return false;

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
					appendPQExpBuffer(firstsql, "REVOKE ALL ON %s %s FROM %s;\n",
									  type, name,
									  fmtId(grantee->data));
					if (privs->len > 0)
						appendPQExpBuffer(firstsql, "GRANT %s ON %s %s TO %s;\n",
										  privs->data, type, name,
										  fmtId(grantee->data));
					if (privswgo->len > 0)
						appendPQExpBuffer(firstsql, "GRANT %s ON %s %s TO %s WITH GRANT OPTION;\n",
										  privswgo->data, type, name,
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
					appendPQExpBuffer(secondsql, "GRANT %s ON %s %s TO ",
									  privs->data, type, name);
					if (grantee->len == 0)
						appendPQExpBuffer(secondsql, "PUBLIC;\n");
					else if (strncmp(grantee->data, "group ",
									 strlen("group ")) == 0)
						appendPQExpBuffer(secondsql, "GROUP %s;\n",
								fmtId(grantee->data + strlen("group ")));
					else
						appendPQExpBuffer(secondsql, "%s;\n", fmtId(grantee->data));
				}
				if (privswgo->len > 0)
				{
					appendPQExpBuffer(secondsql, "GRANT %s ON %s %s TO ",
									  privswgo->data, type, name);
					if (grantee->len == 0)
						appendPQExpBuffer(secondsql, "PUBLIC");
					else if (strncmp(grantee->data, "group ",
									 strlen("group ")) == 0)
						appendPQExpBuffer(secondsql, "GROUP %s",
								fmtId(grantee->data + strlen("group ")));
					else
						appendPQExpBuffer(secondsql, "%s", fmtId(grantee->data));
					appendPQExpBuffer(secondsql, " WITH GRANT OPTION;\n");
				}

				if (grantor->len > 0
					&& (!owner || strcmp(owner, grantor->data) != 0))
					appendPQExpBuffer(secondsql, "RESET SESSION AUTHORIZATION;\n");
			}
		}
	}

	/*
	 * If we didn't find any owner privs, the owner must have revoked 'em
	 * all
	 */
	if (!found_owner_privs && owner)
	{
		appendPQExpBuffer(firstsql, "REVOKE ALL ON %s %s FROM %s;\n",
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

	return true;
}

/*
 * Deconstruct an ACL array (or actually any 1-dimensional Postgres array)
 * into individual items.
 *
 * On success, returns true and sets *itemarray and *nitems to describe
 * an array of individual strings.	On parse failure, returns false;
 * *itemarray may exist or be NULL.
 *
 * NOTE: free'ing itemarray is sufficient to deallocate the working storage.
 */
static bool
parseAclArray(const char *acls, char ***itemarray, int *nitems)
{
	int			inputlen;
	char	  **items;
	char	   *strings;
	int			curitem;

	/*
	 * We expect input in the form of "{item,item,item}" where any item is
	 * either raw data, or surrounded by double quotes (in which case
	 * embedded characters including backslashes and quotes are
	 * backslashed).
	 *
	 * We build the result as an array of pointers followed by the actual
	 * string data, all in one malloc block for convenience of
	 * deallocation. The worst-case storage need is not more than one
	 * pointer and one character for each input character (consider
	 * "{,,,,,,,,,,}").
	 */
	*itemarray = NULL;
	*nitems = 0;
	inputlen = strlen(acls);
	if (inputlen < 2 || acls[0] != '{' || acls[inputlen - 1] != '}')
		return false;			/* bad input */
	items = (char **) malloc(inputlen * (sizeof(char *) + sizeof(char)));
	if (items == NULL)
		return false;			/* out of memory */
	*itemarray = items;
	strings = (char *) (items + inputlen);

	acls++;						/* advance over initial '{' */
	curitem = 0;
	while (*acls != '}')
	{
		if (*acls == '\0')
			return false;		/* premature end of string */
		items[curitem] = strings;
		while (*acls != '}' && *acls != ',')
		{
			if (*acls == '\0')
				return false;	/* premature end of string */
			if (*acls != '"')
				*strings++ = *acls++;	/* copy unquoted data */
			else
			{
				/* process quoted substring */
				acls++;
				while (*acls != '"')
				{
					if (*acls == '\0')
						return false;	/* premature end of string */
					if (*acls == '\\')
					{
						acls++;
						if (*acls == '\0')
							return false;		/* premature end of string */
					}
					*strings++ = *acls++;		/* copy quoted data */
				}
				acls++;
			}
		}
		*strings++ = '\0';
		if (*acls == ',')
			acls++;
		curitem++;
	}
	if (acls[1] != '\0')
		return false;			/* bogus syntax (embedded '}') */
	*nitems = curitem;
	return true;
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
 * the dequoted grantor name or empty.	Privilege characters are decoded
 * and split between privileges with grant option (privswgo) and without
 * (privs).
 *
 * Note: for cross-version compatibility, it's important to use ALL when
 * appropriate.
 */
static bool
parseAclItem(const char *item, const char *type, const char *name,
			 int remoteVersion,
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

	/* user or group name is string up to = */
	eqpos = copyAclUserName(grantee, buf);
	if (*eqpos != '=')
		return false;

	/* grantor may be listed after / */
	slpos = strchr(eqpos + 1, '/');
	if (slpos)
	{
		*slpos++ = '\0';
		slpos = copyAclUserName(grantor, slpos);
		if (*slpos != '\0')
			return false;
	}
	else
		resetPQExpBuffer(grantor);

	/* privilege codes */
#define CONVERT_PRIV(code, keywd) \
	if ((pos = strchr(eqpos + 1, code))) \
	{ \
		if (*(pos + 1) == '*') \
		{ \
			AddAcl(privswgo, keywd); \
			all_without_go = false; \
		} \
		else \
		{ \
			AddAcl(privs, keywd); \
			all_with_go = false; \
		} \
	} \
	else \
		all_with_go = all_without_go = false

	resetPQExpBuffer(privs);
	resetPQExpBuffer(privswgo);

	if (strcmp(type, "TABLE") == 0)
	{
		CONVERT_PRIV('a', "INSERT");
		CONVERT_PRIV('r', "SELECT");
		CONVERT_PRIV('R', "RULE");

		if (remoteVersion >= 70200)
		{
			CONVERT_PRIV('w', "UPDATE");
			CONVERT_PRIV('d', "DELETE");
			CONVERT_PRIV('x', "REFERENCES");
			CONVERT_PRIV('t', "TRIGGER");
		}
		else
		{
			/* 7.0 and 7.1 have a simpler worldview */
			CONVERT_PRIV('w', "UPDATE,DELETE");
		}
	}
	else if (strcmp(type, "FUNCTION") == 0)
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
		CONVERT_PRIV('T', "TEMPORARY");
	}
	else
		abort();

#undef CONVERT_PRIV

	if (all_with_go)
	{
		resetPQExpBuffer(privs);
		printfPQExpBuffer(privswgo, "ALL");
	}
	else if (all_without_go)
	{
		resetPQExpBuffer(privswgo);
		printfPQExpBuffer(privs, "ALL");
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
		/* If user name isn't quoted, then just add it to the output buffer */
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
				 * Quoting convention is to escape " as "".  Keep this
				 * code in sync with putid() in backend's acl.c.
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
AddAcl(PQExpBuffer aclbuf, const char *keyword)
{
	if (aclbuf->len > 0)
		appendPQExpBufferChar(aclbuf, ',');
	appendPQExpBuffer(aclbuf, "%s", keyword);
}
