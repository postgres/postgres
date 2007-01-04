/*-------------------------------------------------------------------------
 *
 * Utility routines for SQL dumping
 *	Basically this is stuff that is useful in both pg_dump and pg_dumpall.
 *	Lately it's also being used by psql and bin/scripts/ ...
 *
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/bin/pg_dump/dumputils.c,v 1.33.2.1 2007/01/04 17:49:42 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include <ctype.h>

#include "dumputils.h"

#include "parser/keywords.h"


#define supports_grant_options(version) ((version) >= 70400)

static bool parseAclItem(const char *item, const char *type, const char *name,
			 int remoteVersion, PQExpBuffer grantee, PQExpBuffer grantor,
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
	 * These checks need to match the identifier production in scan.l. Don't
	 * use islower() etc.
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
 * the given buffer.  We assume the specified client_encoding and
 * standard_conforming_strings settings.
 *
 * This is essentially equivalent to libpq's PQescapeStringInternal,
 * except for the output buffer structure.	We need it in situations
 * where we do not have a PGconn available.  Where we do,
 * appendStringLiteralConn is a better choice.
 */
void
appendStringLiteral(PQExpBuffer buf, const char *str,
					int encoding, bool std_strings)
{
	size_t		length = strlen(str);
	const char *source = str;
	char	   *target;

	if (!enlargePQExpBuffer(buf, 2 * length + 2))
		return;

	target = buf->data + buf->len;
	*target++ = '\'';

	while (*source != '\0')
	{
		char		c = *source;
		int			len;
		int			i;

		/* Fast path for plain ASCII */
		if (!IS_HIGHBIT_SET(c))
		{
			/* Apply quoting if needed */
			if (SQL_STR_DOUBLE(c, !std_strings))
				*target++ = c;
			/* Copy the character */
			*target++ = c;
			source++;
			continue;
		}

		/* Slow path for possible multibyte characters */
		len = PQmblen(source, encoding);

		/* Copy the character */
		for (i = 0; i < len; i++)
		{
			if (*source == '\0')
				break;
			*target++ = *source++;
		}

		/*
		 * If we hit premature end of string (ie, incomplete multibyte
		 * character), try to pad out to the correct length with spaces. We
		 * may not be able to pad completely, but we will always be able to
		 * insert at least one pad space (since we'd not have quoted a
		 * multibyte character).  This should be enough to make a string that
		 * the server will error out on.
		 */
		if (i < len)
		{
			char	   *stop = buf->data + buf->maxlen - 2;

			for (; i < len; i++)
			{
				if (target >= stop)
					break;
				*target++ = ' ';
			}
			break;
		}
	}

	/* Write the terminating quote and NUL character. */
	*target++ = '\'';
	*target = '\0';

	buf->len = target - buf->data;
}


/*
 * Convert a string value to an SQL string literal and append it to
 * the given buffer.  Encoding and string syntax rules are as indicated
 * by current settings of the PGconn.
 */
void
appendStringLiteralConn(PQExpBuffer buf, const char *str, PGconn *conn)
{
	size_t		length = strlen(str);

	/*
	 * XXX This is a kluge to silence escape_string_warning in our utility
	 * programs.  It should go away someday.
	 */
	if (strchr(str, '\\') != NULL && PQserverVersion(conn) >= 80100)
	{
		/* ensure we are not adjacent to an identifier */
		if (buf->len > 0 && buf->data[buf->len - 1] != ' ')
			appendPQExpBufferChar(buf, ' ');
		appendPQExpBufferChar(buf, ESCAPE_STRING_SYNTAX);
		appendStringLiteral(buf, str, PQclientEncoding(conn), false);
		return;
	}
	/* XXX end kluge */

	if (!enlargePQExpBuffer(buf, 2 * length + 2))
		return;
	appendPQExpBufferChar(buf, '\'');
	buf->len += PQescapeStringConn(conn, buf->data + buf->len,
								   str, length, NULL);
	appendPQExpBufferChar(buf, '\'');
}


/*
 * Convert a string value to a dollar quoted literal and append it to
 * the given buffer. If the dqprefix parameter is not NULL then the
 * dollar quote delimiter will begin with that (after the opening $).
 *
 * No escaping is done at all on str, in compliance with the rules
 * for parsing dollar quoted strings.  Also, we need not worry about
 * encoding issues.
 */
void
appendStringLiteralDQ(PQExpBuffer buf, const char *str, const char *dqprefix)
{
	static const char suffixes[] = "_XXXXXXX";
	int			nextchar = 0;
	PQExpBuffer delimBuf = createPQExpBuffer();

	/* start with $ + dqprefix if not NULL */
	appendPQExpBufferChar(delimBuf, '$');
	if (dqprefix)
		appendPQExpBufferStr(delimBuf, dqprefix);

	/*
	 * Make sure we choose a delimiter which (without the trailing $) is not
	 * present in the string being quoted. We don't check with the trailing $
	 * because a string ending in $foo must not be quoted with $foo$.
	 */
	while (strstr(str, delimBuf->data) != NULL)
	{
		appendPQExpBufferChar(delimBuf, suffixes[nextchar++]);
		nextchar %= sizeof(suffixes) - 1;
	}

	/* add trailing $ */
	appendPQExpBufferChar(delimBuf, '$');

	/* quote it and we are all done */
	appendPQExpBufferStr(buf, delimBuf->data);
	appendPQExpBufferStr(buf, str);
	appendPQExpBufferStr(buf, delimBuf->data);

	destroyPQExpBuffer(delimBuf);
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
 * Deconstruct the text representation of a 1-dimensional Postgres array
 * into individual items.
 *
 * On success, returns true and sets *itemarray and *nitems to describe
 * an array of individual strings.	On parse failure, returns false;
 * *itemarray may exist or be NULL.
 *
 * NOTE: free'ing itemarray is sufficient to deallocate the working storage.
 */
bool
parsePGArray(const char *atext, char ***itemarray, int *nitems)
{
	int			inputlen;
	char	  **items;
	char	   *strings;
	int			curitem;

	/*
	 * We expect input in the form of "{item,item,item}" where any item is
	 * either raw data, or surrounded by double quotes (in which case embedded
	 * characters including backslashes and quotes are backslashed).
	 *
	 * We build the result as an array of pointers followed by the actual
	 * string data, all in one malloc block for convenience of deallocation.
	 * The worst-case storage need is not more than one pointer and one
	 * character for each input character (consider "{,,,,,,,,,,}").
	 */
	*itemarray = NULL;
	*nitems = 0;
	inputlen = strlen(atext);
	if (inputlen < 2 || atext[0] != '{' || atext[inputlen - 1] != '}')
		return false;			/* bad input */
	items = (char **) malloc(inputlen * (sizeof(char *) + sizeof(char)));
	if (items == NULL)
		return false;			/* out of memory */
	*itemarray = items;
	strings = (char *) (items + inputlen);

	atext++;					/* advance over initial '{' */
	curitem = 0;
	while (*atext != '}')
	{
		if (*atext == '\0')
			return false;		/* premature end of string */
		items[curitem] = strings;
		while (*atext != '}' && *atext != ',')
		{
			if (*atext == '\0')
				return false;	/* premature end of string */
			if (*atext != '"')
				*strings++ = *atext++;	/* copy unquoted data */
			else
			{
				/* process quoted substring */
				atext++;
				while (*atext != '"')
				{
					if (*atext == '\0')
						return false;	/* premature end of string */
					if (*atext == '\\')
					{
						atext++;
						if (*atext == '\0')
							return false;		/* premature end of string */
					}
					*strings++ = *atext++;		/* copy quoted data */
				}
				atext++;
			}
		}
		*strings++ = '\0';
		if (*atext == ',')
			atext++;
		curitem++;
	}
	if (atext[1] != '\0')
		return false;			/* bogus syntax (embedded '}') */
	*nitems = curitem;
	return true;
}


/*
 * Build GRANT/REVOKE command(s) for an object.
 *
 *	name: the object name, in the form to use in the commands (already quoted)
 *	type: the object type (as seen in GRANT command: must be one of
 *		TABLE, SEQUENCE, FUNCTION, LANGUAGE, SCHEMA, DATABASE, or TABLESPACE)
 *	acls: the ACL string fetched from the database
 *	owner: username of object owner (will be passed through fmtId); can be
 *		NULL or empty string to indicate "no owner known"
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
	 * dependencies valid.	In recent versions this is normally the case, but
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
	appendPQExpBuffer(firstsql, "REVOKE ALL ON %s %s FROM PUBLIC;\n",
					  type, name);

	/*
	 * We still need some hacking though to cover the case where new default
	 * public privileges are added in new versions: the REVOKE ALL will revoke
	 * them, leading to behavior different from what the old version had,
	 * which is generally not what's wanted.  So add back default privs if
	 * the source database is too old to have had that particular priv.
	 */
	if (remoteVersion < 80200 && strcmp(type, "DATABASE") == 0)
	{
		/* database CONNECT priv didn't exist before 8.2 */
		appendPQExpBuffer(firstsql, "GRANT CONNECT ON %s %s TO PUBLIC;\n",
						  type, name);
	}

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
	 * If we didn't find any owner privs, the owner must have revoked 'em all
	 */
	if (!found_owner_privs && owner)
		appendPQExpBuffer(firstsql, "REVOKE ALL ON %s %s FROM %s;\n",
						  type, name, fmtId(owner));

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
			 int remoteVersion, PQExpBuffer grantee, PQExpBuffer grantor,
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
do { \
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
		all_with_go = all_without_go = false; \
} while (0)

	resetPQExpBuffer(privs);
	resetPQExpBuffer(privswgo);

	if (strcmp(type, "TABLE") == 0 || strcmp(type, "SEQUENCE") == 0)
	{
		CONVERT_PRIV('r', "SELECT");

		if (strcmp(type, "SEQUENCE") == 0)
			/* sequence only */
			CONVERT_PRIV('U', "USAGE");
		else
		{
			/* table only */
			CONVERT_PRIV('a', "INSERT");
			if (remoteVersion >= 70200)
			{
				CONVERT_PRIV('d', "DELETE");
				CONVERT_PRIV('x', "REFERENCES");
				CONVERT_PRIV('t', "TRIGGER");
			}
		}

		/* UPDATE */
		if (remoteVersion >= 70200 || strcmp(type, "SEQUENCE") == 0)
			CONVERT_PRIV('w', "UPDATE");
		else
			/* 7.0 and 7.1 have a simpler worldview */
			CONVERT_PRIV('w', "UPDATE,DELETE");
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
		CONVERT_PRIV('c', "CONNECT");
		CONVERT_PRIV('T', "TEMPORARY");
	}
	else if (strcmp(type, "TABLESPACE") == 0)
		CONVERT_PRIV('C', "CREATE");
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
AddAcl(PQExpBuffer aclbuf, const char *keyword)
{
	if (aclbuf->len > 0)
		appendPQExpBufferChar(aclbuf, ',');
	appendPQExpBuffer(aclbuf, "%s", keyword);
}


/*
 * processSQLNamePattern
 *
 * Scan a wildcard-pattern string and generate appropriate WHERE clauses
 * to limit the set of objects returned.  The WHERE clauses are appended
 * to the already-partially-constructed query in buf.
 *
 * conn: connection query will be sent to (consulted for escaping rules).
 * buf: output parameter.
 * pattern: user-specified pattern option, or NULL if none ("*" is implied).
 * have_where: true if caller already emitted "WHERE" (clauses will be ANDed
 * onto the existing WHERE clause).
 * force_escape: always quote regexp special characters, even outside
 * double quotes (else they are quoted only between double quotes).
 * schemavar: name of query variable to match against a schema-name pattern.
 * Can be NULL if no schema.
 * namevar: name of query variable to match against an object-name pattern.
 * altnamevar: NULL, or name of an alternate variable to match against name.
 * visibilityrule: clause to use if we want to restrict to visible objects
 * (for example, "pg_catalog.pg_table_is_visible(p.oid)").	Can be NULL.
 *
 * Formatting note: the text already present in buf should end with a newline.
 * The appended text, if any, will end with one too.
 */
void
processSQLNamePattern(PGconn *conn, PQExpBuffer buf, const char *pattern,
					  bool have_where, bool force_escape,
					  const char *schemavar, const char *namevar,
					  const char *altnamevar, const char *visibilityrule)
{
	PQExpBufferData schemabuf;
	PQExpBufferData namebuf;
	int			encoding = PQclientEncoding(conn);
	bool		inquotes;
	const char *cp;
	int			i;

#define WHEREAND() \
	(appendPQExpBufferStr(buf, have_where ? "  AND " : "WHERE "), have_where = true)

	if (pattern == NULL)
	{
		/* Default: select all visible objects */
		if (visibilityrule)
		{
			WHEREAND();
			appendPQExpBuffer(buf, "%s\n", visibilityrule);
		}
		return;
	}

	initPQExpBuffer(&schemabuf);
	initPQExpBuffer(&namebuf);

	/*
	 * Parse the pattern, converting quotes and lower-casing unquoted letters.
	 * Also, adjust shell-style wildcard characters into regexp notation.
	 *
	 * We surround the pattern with "^(...)$" to force it to match the whole
	 * string, as per SQL practice.  We have to have parens in case the string
	 * contains "|", else the "^" and "$" will be bound into the first and
	 * last alternatives which is not what we want.
	 *
	 * Note: the result of this pass is the actual regexp pattern(s) we want to
	 * execute.  Quoting/escaping into SQL literal format will be done below
	 * using appendStringLiteralConn().
	 */
	appendPQExpBufferStr(&namebuf, "^(");

	inquotes = false;
	cp = pattern;

	while (*cp)
	{
		char	ch = *cp;

		if (ch == '"')
		{
			if (inquotes && cp[1] == '"')
			{
				/* emit one quote, stay in inquotes mode */
				appendPQExpBufferChar(&namebuf, '"');
				cp++;
			}
			else
				inquotes = !inquotes;
			cp++;
		}
		else if (!inquotes && isupper((unsigned char) ch))
		{
			appendPQExpBufferChar(&namebuf,
								  pg_tolower((unsigned char) ch));
			cp++;
		}
		else if (!inquotes && ch == '*')
		{
			appendPQExpBufferStr(&namebuf, ".*");
			cp++;
		}
		else if (!inquotes && ch == '?')
		{
			appendPQExpBufferChar(&namebuf, '.');
			cp++;
		}
		else if (!inquotes && ch == '.')
		{
			/* Found schema/name separator, move current pattern to schema */
			resetPQExpBuffer(&schemabuf);
			appendPQExpBufferStr(&schemabuf, namebuf.data);
			resetPQExpBuffer(&namebuf);
			appendPQExpBufferStr(&namebuf, "^(");
			cp++;
		}
		else
		{
			/*
			 * Ordinary data character, transfer to pattern
			 *
			 * Inside double quotes, or at all times if force_escape is true,
			 * quote regexp special characters with a backslash to avoid
			 * regexp errors.  Outside quotes, however, let them pass through
			 * as-is; this lets knowledgeable users build regexp expressions
			 * that are more powerful than shell-style patterns.
			 */
			if ((inquotes || force_escape) &&
				strchr("|*+?()[]{}.^$\\", ch))
				appendPQExpBufferChar(&namebuf, '\\');
			i = PQmblen(cp, encoding);
			while (i-- && *cp)
			{
				appendPQExpBufferChar(&namebuf, *cp);
				cp++;
			}
		}
	}

	/*
	 * Now decide what we need to emit.  Note there will be a leading "^("
	 * in the patterns in any case.
	 */
	if (namebuf.len > 2)
	{
		/* We have a name pattern, so constrain the namevar(s) */

		appendPQExpBufferStr(&namebuf, ")$");
		/* Optimize away a "*" pattern */
		if (strcmp(namebuf.data, "^(.*)$") != 0)
		{
			WHEREAND();
			if (altnamevar)
			{
				appendPQExpBuffer(buf, "(%s ~ ", namevar);
				appendStringLiteralConn(buf, namebuf.data, conn);
				appendPQExpBuffer(buf, "\n        OR %s ~ ", altnamevar);
				appendStringLiteralConn(buf, namebuf.data, conn);
				appendPQExpBufferStr(buf, ")\n");
			}
			else
			{
				appendPQExpBuffer(buf, "%s ~ ", namevar);
				appendStringLiteralConn(buf, namebuf.data, conn);
				appendPQExpBufferChar(buf, '\n');
			}
		}
	}

	if (schemabuf.len > 2)
	{
		/* We have a schema pattern, so constrain the schemavar */

		appendPQExpBufferStr(&schemabuf, ")$");
		/* Optimize away a "*" pattern */
		if (strcmp(schemabuf.data, "^(.*)$") != 0 && schemavar)
		{
			WHEREAND();
			appendPQExpBuffer(buf, "%s ~ ", schemavar);
			appendStringLiteralConn(buf, schemabuf.data, conn);
			appendPQExpBufferChar(buf, '\n');
		}
	}
	else
	{
		/* No schema pattern given, so select only visible objects */
		if (visibilityrule)
		{
			WHEREAND();
			appendPQExpBuffer(buf, "%s\n", visibilityrule);
		}
	}

	termPQExpBuffer(&schemabuf);
	termPQExpBuffer(&namebuf);

#undef WHEREAND
}
