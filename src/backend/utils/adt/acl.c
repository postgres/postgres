/*-------------------------------------------------------------------------
 *
 * acl.c
 *	  Basic access control list data structures manipulation routines.
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/adt/acl.c,v 1.100 2003/10/29 22:20:54 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>

#include "catalog/namespace.h"
#include "catalog/pg_shadow.h"
#include "catalog/pg_type.h"
#include "commands/dbcommands.h"
#include "miscadmin.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"


#define ACL_IDTYPE_GID_KEYWORD	"group"
#define ACL_IDTYPE_UID_KEYWORD	"user"

static const char *getid(const char *s, char *n);
static void putid(char *p, const char *s);
static Acl *allocacl(int n);
static const char *aclparse(const char *s, AclItem *aip);
static bool aclitem_match(const AclItem *a1, const AclItem *a2);
static Acl *recursive_revoke(Acl *acl, AclId grantee,
				 AclMode revoke_privs, DropBehavior behavior);

static AclMode convert_priv_string(text *priv_type_text);

static Oid	convert_table_name(text *tablename);
static AclMode convert_table_priv_string(text *priv_type_text);
static Oid	convert_database_name(text *databasename);
static AclMode convert_database_priv_string(text *priv_type_text);
static Oid	convert_function_name(text *functionname);
static AclMode convert_function_priv_string(text *priv_type_text);
static Oid	convert_language_name(text *languagename);
static AclMode convert_language_priv_string(text *priv_type_text);
static Oid	convert_schema_name(text *schemaname);
static AclMode convert_schema_priv_string(text *priv_type_text);


/*
 * getid
 *		Consumes the first alphanumeric string (identifier) found in string
 *		's', ignoring any leading white space.	If it finds a double quote
 *		it returns the word inside the quotes.
 *
 * RETURNS:
 *		the string position in 's' that points to the next non-space character
 *		in 's', after any quotes.  Also:
 *		- loads the identifier into 'n'.  (If no identifier is found, 'n'
 *		  contains an empty string.)  'n' must be NAMEDATALEN bytes.
 */
static const char *
getid(const char *s, char *n)
{
	int			len = 0;
	bool		in_quotes = false;

	Assert(s && n);

	while (isspace((unsigned char) *s))
		s++;
	/* This code had better match what putid() does, below */
	for (;
		 *s != '\0' &&
		 (isalnum((unsigned char) *s) ||
		  *s == '_' ||
		  *s == '"' ||
		  in_quotes);
		 s++)
	{
		if (*s == '"')
		{
			/* safe to look at next char (could be '\0' though) */
			if (*(s + 1) != '"')
			{
				in_quotes = !in_quotes;
				continue;
			}
			/* it's an escaped double quote; skip the escaping char */
			s++;
		}

		/* Add the character to the string */
		if (len >= NAMEDATALEN - 1)
			ereport(ERROR,
					(errcode(ERRCODE_NAME_TOO_LONG),
					 errmsg("identifier too long"),
					 errdetail("Identifier must be less than %d characters.",
							   NAMEDATALEN)));

		n[len++] = *s;
	}
	n[len] = '\0';
	while (isspace((unsigned char) *s))
		s++;
	return s;
}

/*
 * Write a user or group Name at *p, adding double quotes if needed.
 * There must be at least (2*NAMEDATALEN)+2 bytes available at *p.
 * This needs to be kept in sync with copyAclUserName in pg_dump/dumputils.c
 */
static void
putid(char *p, const char *s)
{
	const char *src;
	bool		safe = true;

	for (src = s; *src; src++)
	{
		/* This test had better match what getid() does, above */
		if (!isalnum((unsigned char) *src) && *src != '_')
		{
			safe = false;
			break;
		}
	}
	if (!safe)
		*p++ = '"';
	for (src = s; *src; src++)
	{
		/* A double quote character in a username is encoded as "" */
		if (*src == '"')
			*p++ = '"';
		*p++ = *src;
	}
	if (!safe)
		*p++ = '"';
	*p = '\0';
}

/*
 * aclparse
 *		Consumes and parses an ACL specification of the form:
 *				[group|user] [A-Za-z0-9]*=[rwaR]*
 *		from string 's', ignoring any leading white space or white space
 *		between the optional id type keyword (group|user) and the actual
 *		ACL specification.
 *
 *		This routine is called by the parser as well as aclitemin(), hence
 *		the added generality.
 *
 * RETURNS:
 *		the string position in 's' immediately following the ACL
 *		specification.	Also:
 *		- loads the structure pointed to by 'aip' with the appropriate
 *		  UID/GID, id type identifier and mode type values.
 */
static const char *
aclparse(const char *s, AclItem *aip)
{
	AclMode		privs,
				goption,
				read;
	uint32		idtype;
	char		name[NAMEDATALEN];
	char		name2[NAMEDATALEN];

	Assert(s && aip);

#ifdef ACLDEBUG
	elog(LOG, "aclparse: input = \"%s\"", s);
#endif
	idtype = ACL_IDTYPE_UID;
	s = getid(s, name);
	if (*s != '=')
	{
		/* we just read a keyword, not a name */
		if (strcmp(name, ACL_IDTYPE_GID_KEYWORD) == 0)
			idtype = ACL_IDTYPE_GID;
		else if (strcmp(name, ACL_IDTYPE_UID_KEYWORD) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("unrecognized key word: \"%s\"", name),
				 errhint("ACL key word must be \"group\" or \"user\".")));
		s = getid(s, name);		/* move s to the name beyond the keyword */
		if (name[0] == '\0')
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("missing name"),
			   errhint("A name must follow the \"group\" or \"user\" key word.")));
	}
	if (name[0] == '\0')
		idtype = ACL_IDTYPE_WORLD;

	if (*s != '=')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("missing \"=\" sign")));

	privs = goption = ACL_NO_RIGHTS;

	for (++s, read = 0; isalpha((unsigned char) *s) || *s == '*'; s++)
	{
		switch (*s)
		{
			case '*':
				goption |= read;
				break;
			case ACL_INSERT_CHR:
				read = ACL_INSERT;
				break;
			case ACL_SELECT_CHR:
				read = ACL_SELECT;
				break;
			case ACL_UPDATE_CHR:
				read = ACL_UPDATE;
				break;
			case ACL_DELETE_CHR:
				read = ACL_DELETE;
				break;
			case ACL_RULE_CHR:
				read = ACL_RULE;
				break;
			case ACL_REFERENCES_CHR:
				read = ACL_REFERENCES;
				break;
			case ACL_TRIGGER_CHR:
				read = ACL_TRIGGER;
				break;
			case ACL_EXECUTE_CHR:
				read = ACL_EXECUTE;
				break;
			case ACL_USAGE_CHR:
				read = ACL_USAGE;
				break;
			case ACL_CREATE_CHR:
				read = ACL_CREATE;
				break;
			case ACL_CREATE_TEMP_CHR:
				read = ACL_CREATE_TEMP;
				break;
			default:
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				  errmsg("invalid mode character: must be one of \"%s\"",
						 ACL_ALL_RIGHTS_STR)));
		}

		privs |= read;
	}

	switch (idtype)
	{
		case ACL_IDTYPE_UID:
			aip->ai_grantee = get_usesysid(name);
			break;
		case ACL_IDTYPE_GID:
			aip->ai_grantee = get_grosysid(name);
			break;
		case ACL_IDTYPE_WORLD:
			aip->ai_grantee = ACL_ID_WORLD;
			break;
	}

	/*
	 * XXX Allow a degree of backward compatibility by defaulting the
	 * grantor to the superuser.
	 */
	if (*s == '/')
	{
		s = getid(s + 1, name2);
		if (name2[0] == '\0')
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("a name must follow the \"/\" sign")));

		aip->ai_grantor = get_usesysid(name2);
	}
	else
	{
		aip->ai_grantor = BOOTSTRAP_USESYSID;
		ereport(WARNING,
				(errcode(ERRCODE_INVALID_GRANTOR),
				 errmsg("defaulting grantor to user ID %u", BOOTSTRAP_USESYSID)));
	}

	ACLITEM_SET_PRIVS_IDTYPE(*aip, privs, goption, idtype);

#ifdef ACLDEBUG
	elog(LOG, "aclparse: correctly read [%x %d %x]",
		 idtype, aip->ai_grantee, privs);
#endif
	return s;
}

/*
 * allocacl
 *		Allocates storage for a new Acl with 'n' entries.
 *
 * RETURNS:
 *		the new Acl
 */
static Acl *
allocacl(int n)
{
	Acl		   *new_acl;
	Size		size;

	if (n < 0)
		elog(ERROR, "invalid size: %d", n);
	size = ACL_N_SIZE(n);
	new_acl = (Acl *) palloc0(size);
	new_acl->size = size;
	new_acl->ndim = 1;
	new_acl->flags = 0;
	new_acl->elemtype = ACLITEMOID;
	ARR_LBOUND(new_acl)[0] = 0;
	ARR_DIMS(new_acl)[0] = n;
	return new_acl;
}

/*
 * aclitemin
 *		Allocates storage for, and fills in, a new AclItem given a string
 *		's' that contains an ACL specification.  See aclparse for details.
 *
 * RETURNS:
 *		the new AclItem
 */
Datum
aclitemin(PG_FUNCTION_ARGS)
{
	const char *s = PG_GETARG_CSTRING(0);
	AclItem    *aip;

	aip = (AclItem *) palloc(sizeof(AclItem));
	s = aclparse(s, aip);
	while (isspace((unsigned char) *s))
		++s;
	if (*s)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
		   errmsg("extra garbage at the end of the ACL specification")));

	PG_RETURN_ACLITEM_P(aip);
}

/*
 * aclitemout
 *		Allocates storage for, and fills in, a new null-delimited string
 *		containing a formatted ACL specification.  See aclparse for details.
 *
 * RETURNS:
 *		the new string
 */
Datum
aclitemout(PG_FUNCTION_ARGS)
{
	AclItem    *aip = PG_GETARG_ACLITEM_P(0);
	char	   *p;
	char	   *out;
	HeapTuple	htup;
	unsigned	i;
	char	   *tmpname;

	out = palloc(strlen("group =/") +
				 2 * N_ACL_RIGHTS +
				 2 * (2 * NAMEDATALEN + 2) +
				 1);

	p = out;
	*p = '\0';

	switch (ACLITEM_GET_IDTYPE(*aip))
	{
		case ACL_IDTYPE_UID:
			htup = SearchSysCache(SHADOWSYSID,
								  ObjectIdGetDatum(aip->ai_grantee),
								  0, 0, 0);
			if (HeapTupleIsValid(htup))
			{
				putid(p, NameStr(((Form_pg_shadow) GETSTRUCT(htup))->usename));
				ReleaseSysCache(htup);
			}
			else
			{
				/* Generate numeric UID if we don't find an entry */
				sprintf(p, "%d", aip->ai_grantee);
			}
			break;
		case ACL_IDTYPE_GID:
			strcpy(p, "group ");
			p += strlen(p);
			tmpname = get_groname(aip->ai_grantee);
			if (tmpname != NULL)
				putid(p, tmpname);
			else
			{
				/* Generate numeric GID if we don't find an entry */
				sprintf(p, "%d", aip->ai_grantee);
			}
			break;
		case ACL_IDTYPE_WORLD:
			break;
		default:
			elog(ERROR, "unrecognized idtype: %d",
				 (int) ACLITEM_GET_IDTYPE(*aip));
			break;
	}
	while (*p)
		++p;

	*p++ = '=';

	for (i = 0; i < N_ACL_RIGHTS; ++i)
	{
		if (ACLITEM_GET_PRIVS(*aip) & (1 << i))
			*p++ = ACL_ALL_RIGHTS_STR[i];
		if (ACLITEM_GET_GOPTIONS(*aip) & (1 << i))
			*p++ = '*';
	}

	*p++ = '/';
	*p = '\0';

	htup = SearchSysCache(SHADOWSYSID,
						  ObjectIdGetDatum(aip->ai_grantor),
						  0, 0, 0);
	if (HeapTupleIsValid(htup))
	{
		putid(p, NameStr(((Form_pg_shadow) GETSTRUCT(htup))->usename));
		ReleaseSysCache(htup);
	}
	else
	{
		/* Generate numeric UID if we don't find an entry */
		sprintf(p, "%d", aip->ai_grantor);
	}

	while (*p)
		++p;
	*p = '\0';

	PG_RETURN_CSTRING(out);
}

/*
 * aclitem_match
 *		Two AclItems are considered to match iff they have the same
 *		grantee and grantor; the privileges are ignored.
 */
static bool
aclitem_match(const AclItem *a1, const AclItem *a2)
{
	return ACLITEM_GET_IDTYPE(*a1) == ACLITEM_GET_IDTYPE(*a2) &&
		a1->ai_grantee == a2->ai_grantee &&
		a1->ai_grantor == a2->ai_grantor;
}

/*
 * aclitem equality operator
 */
Datum
aclitem_eq(PG_FUNCTION_ARGS)
{
	AclItem    *a1 = PG_GETARG_ACLITEM_P(0);
	AclItem    *a2 = PG_GETARG_ACLITEM_P(1);
	bool		result;

	result = a1->ai_privs == a2->ai_privs &&
		a1->ai_grantee == a2->ai_grantee &&
		a1->ai_grantor == a2->ai_grantor;
	PG_RETURN_BOOL(result);
}

/*
 * aclitem hash function
 *
 * We make aclitems hashable not so much because anyone is likely to hash
 * them, as because we want array equality to work on aclitem arrays, and
 * with the typcache mechanism we must have a hash or btree opclass.
 */
Datum
hash_aclitem(PG_FUNCTION_ARGS)
{
	AclItem    *a = PG_GETARG_ACLITEM_P(0);

	/* not very bright, but avoids any issue of padding in struct */
	PG_RETURN_UINT32((uint32) (a->ai_privs + a->ai_grantee + a->ai_grantor));
}


/*
 * acldefault()  --- create an ACL describing default access permissions
 *
 * Change this routine if you want to alter the default access policy for
 * newly-created objects (or any object with a NULL acl entry).
 */
Acl *
acldefault(GrantObjectType objtype, AclId ownerid)
{
	AclMode		world_default;
	AclMode		owner_default;
	Acl		   *acl;
	AclItem    *aip;

	switch (objtype)
	{
		case ACL_OBJECT_RELATION:
			world_default = ACL_NO_RIGHTS;
			owner_default = ACL_ALL_RIGHTS_RELATION;
			break;
		case ACL_OBJECT_DATABASE:
			world_default = ACL_CREATE_TEMP;	/* not NO_RIGHTS! */
			owner_default = ACL_ALL_RIGHTS_DATABASE;
			break;
		case ACL_OBJECT_FUNCTION:
			/* Grant EXECUTE by default, for now */
			world_default = ACL_EXECUTE;
			owner_default = ACL_ALL_RIGHTS_FUNCTION;
			break;
		case ACL_OBJECT_LANGUAGE:
			/* Grant USAGE by default, for now */
			world_default = ACL_USAGE;
			owner_default = ACL_ALL_RIGHTS_LANGUAGE;
			break;
		case ACL_OBJECT_NAMESPACE:
			world_default = ACL_NO_RIGHTS;
			owner_default = ACL_ALL_RIGHTS_NAMESPACE;
			break;
		default:
			elog(ERROR, "unrecognized objtype: %d", (int) objtype);
			world_default = ACL_NO_RIGHTS;		/* keep compiler quiet */
			owner_default = ACL_NO_RIGHTS;
			break;
	}

	acl = allocacl((world_default != ACL_NO_RIGHTS) ? 2 : 1);
	aip = ACL_DAT(acl);

	if (world_default != ACL_NO_RIGHTS)
	{
		aip->ai_grantee = ACL_ID_WORLD;
		aip->ai_grantor = ownerid;
		ACLITEM_SET_PRIVS_IDTYPE(*aip, world_default, ACL_NO_RIGHTS,
								 ACL_IDTYPE_WORLD);
		aip++;
	}

	aip->ai_grantee = ownerid;
	aip->ai_grantor = ownerid;
	/* owner gets default privileges with grant option */
	ACLITEM_SET_PRIVS_IDTYPE(*aip, owner_default, owner_default,
							 ACL_IDTYPE_UID);

	return acl;
}


/*
 * Add or replace an item in an ACL array.	The result is a modified copy;
 * the input object is not changed.
 *
 * NB: caller is responsible for having detoasted the input ACL, if needed.
 */
Acl *
aclinsert3(const Acl *old_acl, const AclItem *mod_aip,
		   unsigned modechg, DropBehavior behavior)
{
	Acl		   *new_acl = NULL;
	AclItem    *old_aip,
			   *new_aip = NULL;
	AclMode		old_privs,
				old_goptions,
				new_privs,
				new_goptions;
	int			dst,
				num;

	/* These checks for null input are probably dead code, but... */
	if (!old_acl || ACL_NUM(old_acl) < 0)
		old_acl = allocacl(0);
	if (!mod_aip)
	{
		new_acl = allocacl(ACL_NUM(old_acl));
		memcpy((char *) new_acl, (char *) old_acl, ACL_SIZE(old_acl));
		return new_acl;
	}

	num = ACL_NUM(old_acl);
	old_aip = ACL_DAT(old_acl);

	/*
	 * Search the ACL for an existing entry for this grantee and grantor.
	 * If one exists, just modify the entry in-place (well, in the same
	 * position, since we actually return a copy); otherwise, insert the
	 * new entry at the end.
	 */

	for (dst = 0; dst < num; ++dst)
	{
		if (aclitem_match(mod_aip, old_aip + dst))
		{
			/* found a match, so modify existing item */
			new_acl = allocacl(num);
			new_aip = ACL_DAT(new_acl);
			memcpy(new_acl, old_acl, ACL_SIZE(old_acl));
			break;
		}
	}

	if (dst == num)
	{
		/* need to append a new item */
		new_acl = allocacl(num + 1);
		new_aip = ACL_DAT(new_acl);
		memcpy(new_aip, old_aip, num * sizeof(AclItem));

		/* initialize the new entry with no permissions */
		new_aip[dst].ai_grantee = mod_aip->ai_grantee;
		new_aip[dst].ai_grantor = mod_aip->ai_grantor;
		ACLITEM_SET_PRIVS_IDTYPE(new_aip[dst], ACL_NO_RIGHTS, ACL_NO_RIGHTS,
								 ACLITEM_GET_IDTYPE(*mod_aip));
		num++;					/* set num to the size of new_acl */
	}

	old_privs = ACLITEM_GET_PRIVS(new_aip[dst]);
	old_goptions = ACLITEM_GET_GOPTIONS(new_aip[dst]);

	/* apply the specified permissions change */
	switch (modechg)
	{
		case ACL_MODECHG_ADD:
			ACLITEM_SET_PRIVS(new_aip[dst],
							  old_privs | ACLITEM_GET_PRIVS(*mod_aip));
			ACLITEM_SET_GOPTIONS(new_aip[dst],
								 old_goptions | ACLITEM_GET_GOPTIONS(*mod_aip));
			break;
		case ACL_MODECHG_DEL:
			ACLITEM_SET_PRIVS(new_aip[dst],
							  old_privs & ~ACLITEM_GET_PRIVS(*mod_aip));
			ACLITEM_SET_GOPTIONS(new_aip[dst],
								 old_goptions & ~ACLITEM_GET_GOPTIONS(*mod_aip));
			break;
		case ACL_MODECHG_EQL:
			ACLITEM_SET_PRIVS_IDTYPE(new_aip[dst],
									 ACLITEM_GET_PRIVS(*mod_aip),
									 ACLITEM_GET_GOPTIONS(*mod_aip),
									 ACLITEM_GET_IDTYPE(new_aip[dst]));
			break;
	}

	new_privs = ACLITEM_GET_PRIVS(new_aip[dst]);
	new_goptions = ACLITEM_GET_GOPTIONS(new_aip[dst]);

	/*
	 * If the adjusted entry has no permissions, delete it from the list.
	 */
	if (new_privs == ACL_NO_RIGHTS && new_goptions == ACL_NO_RIGHTS)
	{
		memmove(new_aip + dst,
				new_aip + dst + 1,
				(num - dst - 1) * sizeof(AclItem));
		ARR_DIMS(new_acl)[0] = num - 1;
		ARR_SIZE(new_acl) -= sizeof(AclItem);
	}

	/*
	 * Remove abandoned privileges (cascading revoke).  Currently we
	 * can only handle this when the grantee is a user.
	 */
	if ((old_goptions & ~new_goptions) != 0
		&& ACLITEM_GET_IDTYPE(*mod_aip) == ACL_IDTYPE_UID)
		new_acl = recursive_revoke(new_acl, mod_aip->ai_grantee,
								   (old_goptions & ~new_goptions),
								   behavior);

	return new_acl;
}


/*
 * Ensure that no privilege is "abandoned".  A privilege is abandoned
 * if the user that granted the privilege loses the grant option.  (So
 * the chain through which it was granted is broken.)  Either the
 * abandoned privileges are revoked as well, or an error message is
 * printed, depending on the drop behavior option.
 */
static Acl *
recursive_revoke(Acl *acl,
				 AclId grantee,
				 AclMode revoke_privs,
				 DropBehavior behavior)
{
	int			i;

restart:
	for (i = 0; i < ACL_NUM(acl); i++)
	{
		AclItem    *aip = ACL_DAT(acl);

		if (aip[i].ai_grantor == grantee
			&& (ACLITEM_GET_PRIVS(aip[i]) & revoke_privs) != 0)
		{
			AclItem		mod_acl;

			if (behavior == DROP_RESTRICT)
				ereport(ERROR,
						(errcode(ERRCODE_DEPENDENT_OBJECTS_STILL_EXIST),
						 errmsg("dependent privileges exist"),
						 errhint("Use CASCADE to revoke them too.")));

			mod_acl.ai_grantor = grantee;
			mod_acl.ai_grantee = aip[i].ai_grantee;
			ACLITEM_SET_PRIVS_IDTYPE(mod_acl,
									 revoke_privs,
									 revoke_privs,
									 ACLITEM_GET_IDTYPE(aip[i]));

			acl = aclinsert3(acl, &mod_acl, ACL_MODECHG_DEL, behavior);
			goto restart;
		}
	}

	return acl;
}


/*
 * aclinsert (exported function)
 */
Datum
aclinsert(PG_FUNCTION_ARGS)
{
	Acl		   *old_acl = PG_GETARG_ACL_P(0);
	AclItem    *mod_aip = PG_GETARG_ACLITEM_P(1);

	PG_RETURN_ACL_P(aclinsert3(old_acl, mod_aip, ACL_MODECHG_EQL, DROP_CASCADE));
}

Datum
aclremove(PG_FUNCTION_ARGS)
{
	Acl		   *old_acl = PG_GETARG_ACL_P(0);
	AclItem    *mod_aip = PG_GETARG_ACLITEM_P(1);
	Acl		   *new_acl;
	AclItem    *old_aip,
			   *new_aip;
	int			dst,
				old_num,
				new_num;

	/* These checks for null input should be dead code, but... */
	if (!old_acl || ACL_NUM(old_acl) < 0)
		old_acl = allocacl(0);
	if (!mod_aip)
	{
		new_acl = allocacl(ACL_NUM(old_acl));
		memcpy((char *) new_acl, (char *) old_acl, ACL_SIZE(old_acl));
		PG_RETURN_ACL_P(new_acl);
	}

	old_num = ACL_NUM(old_acl);
	old_aip = ACL_DAT(old_acl);

	/* Search for the matching entry */
	for (dst = 0;
		 dst < old_num && !aclitem_match(mod_aip, old_aip + dst);
		 ++dst)
		 /* continue */ ;

	if (dst >= old_num)
	{
		/* Not found, so return copy of source ACL */
		new_acl = allocacl(old_num);
		memcpy((char *) new_acl, (char *) old_acl, ACL_SIZE(old_acl));
	}
	else
	{
		new_num = old_num - 1;
		new_acl = allocacl(new_num);
		new_aip = ACL_DAT(new_acl);
		if (dst > 0)
			memcpy((char *) new_aip,
				   (char *) old_aip,
				   dst * sizeof(AclItem));
		if (dst < new_num)
			memcpy((char *) (new_aip + dst),
				   (char *) (old_aip + dst + 1),
				   (new_num - dst) * sizeof(AclItem));
	}

	PG_RETURN_ACL_P(new_acl);
}

Datum
aclcontains(PG_FUNCTION_ARGS)
{
	Acl		   *acl = PG_GETARG_ACL_P(0);
	AclItem    *aip = PG_GETARG_ACLITEM_P(1);
	AclItem    *aidat;
	int			i,
				num;

	num = ACL_NUM(acl);
	aidat = ACL_DAT(acl);
	for (i = 0; i < num; ++i)
	{
		if (aip->ai_grantee == aidat[i].ai_grantee
			&& ACLITEM_GET_IDTYPE(*aip) == ACLITEM_GET_IDTYPE(aidat[i])
			&& aip->ai_grantor == aidat[i].ai_grantor
			&& (ACLITEM_GET_PRIVS(*aip) & ACLITEM_GET_PRIVS(aidat[i])) == ACLITEM_GET_PRIVS(*aip)
			&& (ACLITEM_GET_GOPTIONS(*aip) & ACLITEM_GET_GOPTIONS(aidat[i])) == ACLITEM_GET_GOPTIONS(*aip))
			PG_RETURN_BOOL(true);
	}
	PG_RETURN_BOOL(false);
}

Datum
makeaclitem(PG_FUNCTION_ARGS)
{
	int32		u_grantee = PG_GETARG_INT32(0);
	int32		g_grantee = PG_GETARG_INT32(1);
	int32		grantor = PG_GETARG_INT32(2);
	text	   *privtext = PG_GETARG_TEXT_P(3);
	bool		goption = PG_GETARG_BOOL(4);
	AclItem    *aclitem;
	AclMode		priv;

	priv = convert_priv_string(privtext);

	aclitem = (AclItem *) palloc(sizeof(*aclitem));

	if (u_grantee == 0 && g_grantee == 0)
	{
		aclitem->ai_grantee = ACL_ID_WORLD;

		ACLITEM_SET_IDTYPE(*aclitem, ACL_IDTYPE_WORLD);
	}
	else if (u_grantee != 0 && g_grantee != 0)
	{
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("cannot specify both user and group")));
	}
	else if (u_grantee != 0)
	{
		aclitem->ai_grantee = u_grantee;

		ACLITEM_SET_IDTYPE(*aclitem, ACL_IDTYPE_UID);
	}
	else /* (g_grantee != 0) */
	{
		aclitem->ai_grantee = g_grantee;

		ACLITEM_SET_IDTYPE(*aclitem, ACL_IDTYPE_GID);
	}

	aclitem->ai_grantor = grantor;

	ACLITEM_SET_PRIVS(*aclitem, priv);
	if (goption)
		ACLITEM_SET_GOPTIONS(*aclitem, priv);
	else
		ACLITEM_SET_GOPTIONS(*aclitem, ACL_NO_RIGHTS);

	PG_RETURN_ACLITEM_P(aclitem);
}

static AclMode
convert_priv_string(text *priv_type_text)
{
	char	   *priv_type;

	priv_type = DatumGetCString(DirectFunctionCall1(textout,
									   PointerGetDatum(priv_type_text)));

	if (strcasecmp(priv_type, "SELECT") == 0)
		return ACL_SELECT;
	if (strcasecmp(priv_type, "INSERT") == 0)
		return ACL_INSERT;
	if (strcasecmp(priv_type, "UPDATE") == 0)
		return ACL_UPDATE;
	if (strcasecmp(priv_type, "DELETE") == 0)
		return ACL_DELETE;
	if (strcasecmp(priv_type, "RULE") == 0)
		return ACL_RULE;
	if (strcasecmp(priv_type, "REFERENCES") == 0)
		return ACL_REFERENCES;
	if (strcasecmp(priv_type, "TRIGGER") == 0)
		return ACL_TRIGGER;
	if (strcasecmp(priv_type, "EXECUTE") == 0)
		return ACL_EXECUTE;
	if (strcasecmp(priv_type, "USAGE") == 0)
		return ACL_USAGE;
	if (strcasecmp(priv_type, "CREATE") == 0)
		return ACL_CREATE;
	if (strcasecmp(priv_type, "TEMP") == 0)
		return ACL_CREATE_TEMP;
	if (strcasecmp(priv_type, "TEMPORARY") == 0)
		return ACL_CREATE_TEMP;

	ereport(ERROR,
			(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			 errmsg("unrecognized privilege type: \"%s\"", priv_type)));
	return ACL_NO_RIGHTS;		/* keep compiler quiet */
}


/*
 * has_table_privilege variants
 *		These are all named "has_table_privilege" at the SQL level.
 *		They take various combinations of relation name, relation OID,
 *		user name, user sysid, or implicit user = current_user.
 *
 *		The result is a boolean value: true if user has the indicated
 *		privilege, false if not.
 */

/*
 * has_table_privilege_name_name
 *		Check user privileges on a table given
 *		name username, text tablename, and text priv name.
 */
Datum
has_table_privilege_name_name(PG_FUNCTION_ARGS)
{
	Name		username = PG_GETARG_NAME(0);
	text	   *tablename = PG_GETARG_TEXT_P(1);
	text	   *priv_type_text = PG_GETARG_TEXT_P(2);
	int32		usesysid;
	Oid			tableoid;
	AclMode		mode;
	AclResult	aclresult;

	usesysid = get_usesysid(NameStr(*username));
	tableoid = convert_table_name(tablename);
	mode = convert_table_priv_string(priv_type_text);

	aclresult = pg_class_aclcheck(tableoid, usesysid, mode);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}

/*
 * has_table_privilege_name
 *		Check user privileges on a table given
 *		text tablename and text priv name.
 *		current_user is assumed
 */
Datum
has_table_privilege_name(PG_FUNCTION_ARGS)
{
	text	   *tablename = PG_GETARG_TEXT_P(0);
	text	   *priv_type_text = PG_GETARG_TEXT_P(1);
	AclId		usesysid;
	Oid			tableoid;
	AclMode		mode;
	AclResult	aclresult;

	usesysid = GetUserId();
	tableoid = convert_table_name(tablename);
	mode = convert_table_priv_string(priv_type_text);

	aclresult = pg_class_aclcheck(tableoid, usesysid, mode);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}

/*
 * has_table_privilege_name_id
 *		Check user privileges on a table given
 *		name usename, table oid, and text priv name.
 */
Datum
has_table_privilege_name_id(PG_FUNCTION_ARGS)
{
	Name		username = PG_GETARG_NAME(0);
	Oid			tableoid = PG_GETARG_OID(1);
	text	   *priv_type_text = PG_GETARG_TEXT_P(2);
	int32		usesysid;
	AclMode		mode;
	AclResult	aclresult;

	usesysid = get_usesysid(NameStr(*username));
	mode = convert_table_priv_string(priv_type_text);

	aclresult = pg_class_aclcheck(tableoid, usesysid, mode);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}

/*
 * has_table_privilege_id
 *		Check user privileges on a table given
 *		table oid, and text priv name.
 *		current_user is assumed
 */
Datum
has_table_privilege_id(PG_FUNCTION_ARGS)
{
	Oid			tableoid = PG_GETARG_OID(0);
	text	   *priv_type_text = PG_GETARG_TEXT_P(1);
	AclId		usesysid;
	AclMode		mode;
	AclResult	aclresult;

	usesysid = GetUserId();
	mode = convert_table_priv_string(priv_type_text);

	aclresult = pg_class_aclcheck(tableoid, usesysid, mode);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}

/*
 * has_table_privilege_id_name
 *		Check user privileges on a table given
 *		usesysid, text tablename, and text priv name.
 */
Datum
has_table_privilege_id_name(PG_FUNCTION_ARGS)
{
	int32		usesysid = PG_GETARG_INT32(0);
	text	   *tablename = PG_GETARG_TEXT_P(1);
	text	   *priv_type_text = PG_GETARG_TEXT_P(2);
	Oid			tableoid;
	AclMode		mode;
	AclResult	aclresult;

	tableoid = convert_table_name(tablename);
	mode = convert_table_priv_string(priv_type_text);

	aclresult = pg_class_aclcheck(tableoid, usesysid, mode);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}

/*
 * has_table_privilege_id_id
 *		Check user privileges on a table given
 *		usesysid, table oid, and text priv name.
 */
Datum
has_table_privilege_id_id(PG_FUNCTION_ARGS)
{
	int32		usesysid = PG_GETARG_INT32(0);
	Oid			tableoid = PG_GETARG_OID(1);
	text	   *priv_type_text = PG_GETARG_TEXT_P(2);
	AclMode		mode;
	AclResult	aclresult;

	mode = convert_table_priv_string(priv_type_text);

	aclresult = pg_class_aclcheck(tableoid, usesysid, mode);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}

/*
 *		Support routines for has_table_privilege family.
 */

/*
 * Given a table name expressed as a string, look it up and return Oid
 */
static Oid
convert_table_name(text *tablename)
{
	RangeVar   *relrv;

	relrv = makeRangeVarFromNameList(textToQualifiedNameList(tablename,
												 "has_table_privilege"));

	return RangeVarGetRelid(relrv, false);
}

/*
 * convert_table_priv_string
 *		Convert text string to AclMode value.
 */
static AclMode
convert_table_priv_string(text *priv_type_text)
{
	char	   *priv_type;

	priv_type = DatumGetCString(DirectFunctionCall1(textout,
									   PointerGetDatum(priv_type_text)));

	/*
	 * Return mode from priv_type string
	 */
	if (strcasecmp(priv_type, "SELECT") == 0)
		return ACL_SELECT;
	if (strcasecmp(priv_type, "SELECT WITH GRANT OPTION") == 0)
		return ACL_GRANT_OPTION_FOR(ACL_SELECT);

	if (strcasecmp(priv_type, "INSERT") == 0)
		return ACL_INSERT;
	if (strcasecmp(priv_type, "INSERT WITH GRANT OPTION") == 0)
		return ACL_GRANT_OPTION_FOR(ACL_INSERT);

	if (strcasecmp(priv_type, "UPDATE") == 0)
		return ACL_UPDATE;
	if (strcasecmp(priv_type, "UPDATE WITH GRANT OPTION") == 0)
		return ACL_GRANT_OPTION_FOR(ACL_UPDATE);

	if (strcasecmp(priv_type, "DELETE") == 0)
		return ACL_DELETE;
	if (strcasecmp(priv_type, "DELETE WITH GRANT OPTION") == 0)
		return ACL_GRANT_OPTION_FOR(ACL_DELETE);

	if (strcasecmp(priv_type, "RULE") == 0)
		return ACL_RULE;
	if (strcasecmp(priv_type, "RULE WITH GRANT OPTION") == 0)
		return ACL_GRANT_OPTION_FOR(ACL_RULE);

	if (strcasecmp(priv_type, "REFERENCES") == 0)
		return ACL_REFERENCES;
	if (strcasecmp(priv_type, "REFERENCES WITH GRANT OPTION") == 0)
		return ACL_GRANT_OPTION_FOR(ACL_REFERENCES);

	if (strcasecmp(priv_type, "TRIGGER") == 0)
		return ACL_TRIGGER;
	if (strcasecmp(priv_type, "TRIGGER WITH GRANT OPTION") == 0)
		return ACL_GRANT_OPTION_FOR(ACL_TRIGGER);

	ereport(ERROR,
			(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			 errmsg("unrecognized privilege type: \"%s\"", priv_type)));
	return ACL_NO_RIGHTS;		/* keep compiler quiet */
}


/*
 * has_database_privilege variants
 *		These are all named "has_database_privilege" at the SQL level.
 *		They take various combinations of database name, database OID,
 *		user name, user sysid, or implicit user = current_user.
 *
 *		The result is a boolean value: true if user has the indicated
 *		privilege, false if not.
 */

/*
 * has_database_privilege_name_name
 *		Check user privileges on a database given
 *		name username, text databasename, and text priv name.
 */
Datum
has_database_privilege_name_name(PG_FUNCTION_ARGS)
{
	Name		username = PG_GETARG_NAME(0);
	text	   *databasename = PG_GETARG_TEXT_P(1);
	text	   *priv_type_text = PG_GETARG_TEXT_P(2);
	int32		usesysid;
	Oid			databaseoid;
	AclMode		mode;
	AclResult	aclresult;

	usesysid = get_usesysid(NameStr(*username));
	databaseoid = convert_database_name(databasename);
	mode = convert_database_priv_string(priv_type_text);

	aclresult = pg_database_aclcheck(databaseoid, usesysid, mode);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}

/*
 * has_database_privilege_name
 *		Check user privileges on a database given
 *		text databasename and text priv name.
 *		current_user is assumed
 */
Datum
has_database_privilege_name(PG_FUNCTION_ARGS)
{
	text	   *databasename = PG_GETARG_TEXT_P(0);
	text	   *priv_type_text = PG_GETARG_TEXT_P(1);
	AclId		usesysid;
	Oid			databaseoid;
	AclMode		mode;
	AclResult	aclresult;

	usesysid = GetUserId();
	databaseoid = convert_database_name(databasename);
	mode = convert_database_priv_string(priv_type_text);

	aclresult = pg_database_aclcheck(databaseoid, usesysid, mode);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}

/*
 * has_database_privilege_name_id
 *		Check user privileges on a database given
 *		name usename, database oid, and text priv name.
 */
Datum
has_database_privilege_name_id(PG_FUNCTION_ARGS)
{
	Name		username = PG_GETARG_NAME(0);
	Oid			databaseoid = PG_GETARG_OID(1);
	text	   *priv_type_text = PG_GETARG_TEXT_P(2);
	int32		usesysid;
	AclMode		mode;
	AclResult	aclresult;

	usesysid = get_usesysid(NameStr(*username));
	mode = convert_database_priv_string(priv_type_text);

	aclresult = pg_database_aclcheck(databaseoid, usesysid, mode);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}

/*
 * has_database_privilege_id
 *		Check user privileges on a database given
 *		database oid, and text priv name.
 *		current_user is assumed
 */
Datum
has_database_privilege_id(PG_FUNCTION_ARGS)
{
	Oid			databaseoid = PG_GETARG_OID(0);
	text	   *priv_type_text = PG_GETARG_TEXT_P(1);
	AclId		usesysid;
	AclMode		mode;
	AclResult	aclresult;

	usesysid = GetUserId();
	mode = convert_database_priv_string(priv_type_text);

	aclresult = pg_database_aclcheck(databaseoid, usesysid, mode);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}

/*
 * has_database_privilege_id_name
 *		Check user privileges on a database given
 *		usesysid, text databasename, and text priv name.
 */
Datum
has_database_privilege_id_name(PG_FUNCTION_ARGS)
{
	int32		usesysid = PG_GETARG_INT32(0);
	text	   *databasename = PG_GETARG_TEXT_P(1);
	text	   *priv_type_text = PG_GETARG_TEXT_P(2);
	Oid			databaseoid;
	AclMode		mode;
	AclResult	aclresult;

	databaseoid = convert_database_name(databasename);
	mode = convert_database_priv_string(priv_type_text);

	aclresult = pg_database_aclcheck(databaseoid, usesysid, mode);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}

/*
 * has_database_privilege_id_id
 *		Check user privileges on a database given
 *		usesysid, database oid, and text priv name.
 */
Datum
has_database_privilege_id_id(PG_FUNCTION_ARGS)
{
	int32		usesysid = PG_GETARG_INT32(0);
	Oid			databaseoid = PG_GETARG_OID(1);
	text	   *priv_type_text = PG_GETARG_TEXT_P(2);
	AclMode		mode;
	AclResult	aclresult;

	mode = convert_database_priv_string(priv_type_text);

	aclresult = pg_database_aclcheck(databaseoid, usesysid, mode);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}

/*
 *		Support routines for has_database_privilege family.
 */

/*
 * Given a database name expressed as a string, look it up and return Oid
 */
static Oid
convert_database_name(text *databasename)
{
	char	   *dbname;
	Oid			oid;

	dbname = DatumGetCString(DirectFunctionCall1(textout,
										 PointerGetDatum(databasename)));

	oid = get_database_oid(dbname);
	if (!OidIsValid(oid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_DATABASE),
				 errmsg("database \"%s\" does not exist", dbname)));

	return oid;
}

/*
 * convert_database_priv_string
 *		Convert text string to AclMode value.
 */
static AclMode
convert_database_priv_string(text *priv_type_text)
{
	char	   *priv_type;

	priv_type = DatumGetCString(DirectFunctionCall1(textout,
									   PointerGetDatum(priv_type_text)));

	/*
	 * Return mode from priv_type string
	 */
	if (strcasecmp(priv_type, "CREATE") == 0)
		return ACL_CREATE;
	if (strcasecmp(priv_type, "CREATE WITH GRANT OPTION") == 0)
		return ACL_GRANT_OPTION_FOR(ACL_CREATE);

	if (strcasecmp(priv_type, "TEMPORARY") == 0)
		return ACL_CREATE_TEMP;
	if (strcasecmp(priv_type, "TEMPORARY WITH GRANT OPTION") == 0)
		return ACL_GRANT_OPTION_FOR(ACL_CREATE_TEMP);

	if (strcasecmp(priv_type, "TEMP") == 0)
		return ACL_CREATE_TEMP;
	if (strcasecmp(priv_type, "TEMP WITH GRANT OPTION") == 0)
		return ACL_GRANT_OPTION_FOR(ACL_CREATE_TEMP);

	ereport(ERROR,
			(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			 errmsg("unrecognized privilege type: \"%s\"", priv_type)));
	return ACL_NO_RIGHTS;		/* keep compiler quiet */
}


/*
 * has_function_privilege variants
 *		These are all named "has_function_privilege" at the SQL level.
 *		They take various combinations of function name, function OID,
 *		user name, user sysid, or implicit user = current_user.
 *
 *		The result is a boolean value: true if user has the indicated
 *		privilege, false if not.
 */

/*
 * has_function_privilege_name_name
 *		Check user privileges on a function given
 *		name username, text functionname, and text priv name.
 */
Datum
has_function_privilege_name_name(PG_FUNCTION_ARGS)
{
	Name		username = PG_GETARG_NAME(0);
	text	   *functionname = PG_GETARG_TEXT_P(1);
	text	   *priv_type_text = PG_GETARG_TEXT_P(2);
	int32		usesysid;
	Oid			functionoid;
	AclMode		mode;
	AclResult	aclresult;

	usesysid = get_usesysid(NameStr(*username));
	functionoid = convert_function_name(functionname);
	mode = convert_function_priv_string(priv_type_text);

	aclresult = pg_proc_aclcheck(functionoid, usesysid, mode);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}

/*
 * has_function_privilege_name
 *		Check user privileges on a function given
 *		text functionname and text priv name.
 *		current_user is assumed
 */
Datum
has_function_privilege_name(PG_FUNCTION_ARGS)
{
	text	   *functionname = PG_GETARG_TEXT_P(0);
	text	   *priv_type_text = PG_GETARG_TEXT_P(1);
	AclId		usesysid;
	Oid			functionoid;
	AclMode		mode;
	AclResult	aclresult;

	usesysid = GetUserId();
	functionoid = convert_function_name(functionname);
	mode = convert_function_priv_string(priv_type_text);

	aclresult = pg_proc_aclcheck(functionoid, usesysid, mode);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}

/*
 * has_function_privilege_name_id
 *		Check user privileges on a function given
 *		name usename, function oid, and text priv name.
 */
Datum
has_function_privilege_name_id(PG_FUNCTION_ARGS)
{
	Name		username = PG_GETARG_NAME(0);
	Oid			functionoid = PG_GETARG_OID(1);
	text	   *priv_type_text = PG_GETARG_TEXT_P(2);
	int32		usesysid;
	AclMode		mode;
	AclResult	aclresult;

	usesysid = get_usesysid(NameStr(*username));
	mode = convert_function_priv_string(priv_type_text);

	aclresult = pg_proc_aclcheck(functionoid, usesysid, mode);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}

/*
 * has_function_privilege_id
 *		Check user privileges on a function given
 *		function oid, and text priv name.
 *		current_user is assumed
 */
Datum
has_function_privilege_id(PG_FUNCTION_ARGS)
{
	Oid			functionoid = PG_GETARG_OID(0);
	text	   *priv_type_text = PG_GETARG_TEXT_P(1);
	AclId		usesysid;
	AclMode		mode;
	AclResult	aclresult;

	usesysid = GetUserId();
	mode = convert_function_priv_string(priv_type_text);

	aclresult = pg_proc_aclcheck(functionoid, usesysid, mode);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}

/*
 * has_function_privilege_id_name
 *		Check user privileges on a function given
 *		usesysid, text functionname, and text priv name.
 */
Datum
has_function_privilege_id_name(PG_FUNCTION_ARGS)
{
	int32		usesysid = PG_GETARG_INT32(0);
	text	   *functionname = PG_GETARG_TEXT_P(1);
	text	   *priv_type_text = PG_GETARG_TEXT_P(2);
	Oid			functionoid;
	AclMode		mode;
	AclResult	aclresult;

	functionoid = convert_function_name(functionname);
	mode = convert_function_priv_string(priv_type_text);

	aclresult = pg_proc_aclcheck(functionoid, usesysid, mode);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}

/*
 * has_function_privilege_id_id
 *		Check user privileges on a function given
 *		usesysid, function oid, and text priv name.
 */
Datum
has_function_privilege_id_id(PG_FUNCTION_ARGS)
{
	int32		usesysid = PG_GETARG_INT32(0);
	Oid			functionoid = PG_GETARG_OID(1);
	text	   *priv_type_text = PG_GETARG_TEXT_P(2);
	AclMode		mode;
	AclResult	aclresult;

	mode = convert_function_priv_string(priv_type_text);

	aclresult = pg_proc_aclcheck(functionoid, usesysid, mode);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}

/*
 *		Support routines for has_function_privilege family.
 */

/*
 * Given a function name expressed as a string, look it up and return Oid
 */
static Oid
convert_function_name(text *functionname)
{
	char	   *funcname;
	Oid			oid;

	funcname = DatumGetCString(DirectFunctionCall1(textout,
										 PointerGetDatum(functionname)));

	oid = DatumGetObjectId(DirectFunctionCall1(regprocedurein,
											 CStringGetDatum(funcname)));

	if (!OidIsValid(oid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("function \"%s\" does not exist", funcname)));

	return oid;
}

/*
 * convert_function_priv_string
 *		Convert text string to AclMode value.
 */
static AclMode
convert_function_priv_string(text *priv_type_text)
{
	char	   *priv_type;

	priv_type = DatumGetCString(DirectFunctionCall1(textout,
									   PointerGetDatum(priv_type_text)));

	/*
	 * Return mode from priv_type string
	 */
	if (strcasecmp(priv_type, "EXECUTE") == 0)
		return ACL_EXECUTE;
	if (strcasecmp(priv_type, "EXECUTE WITH GRANT OPTION") == 0)
		return ACL_GRANT_OPTION_FOR(ACL_EXECUTE);

	ereport(ERROR,
			(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			 errmsg("unrecognized privilege type: \"%s\"", priv_type)));
	return ACL_NO_RIGHTS;		/* keep compiler quiet */
}


/*
 * has_language_privilege variants
 *		These are all named "has_language_privilege" at the SQL level.
 *		They take various combinations of language name, language OID,
 *		user name, user sysid, or implicit user = current_user.
 *
 *		The result is a boolean value: true if user has the indicated
 *		privilege, false if not.
 */

/*
 * has_language_privilege_name_name
 *		Check user privileges on a language given
 *		name username, text languagename, and text priv name.
 */
Datum
has_language_privilege_name_name(PG_FUNCTION_ARGS)
{
	Name		username = PG_GETARG_NAME(0);
	text	   *languagename = PG_GETARG_TEXT_P(1);
	text	   *priv_type_text = PG_GETARG_TEXT_P(2);
	int32		usesysid;
	Oid			languageoid;
	AclMode		mode;
	AclResult	aclresult;

	usesysid = get_usesysid(NameStr(*username));
	languageoid = convert_language_name(languagename);
	mode = convert_language_priv_string(priv_type_text);

	aclresult = pg_language_aclcheck(languageoid, usesysid, mode);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}

/*
 * has_language_privilege_name
 *		Check user privileges on a language given
 *		text languagename and text priv name.
 *		current_user is assumed
 */
Datum
has_language_privilege_name(PG_FUNCTION_ARGS)
{
	text	   *languagename = PG_GETARG_TEXT_P(0);
	text	   *priv_type_text = PG_GETARG_TEXT_P(1);
	AclId		usesysid;
	Oid			languageoid;
	AclMode		mode;
	AclResult	aclresult;

	usesysid = GetUserId();
	languageoid = convert_language_name(languagename);
	mode = convert_language_priv_string(priv_type_text);

	aclresult = pg_language_aclcheck(languageoid, usesysid, mode);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}

/*
 * has_language_privilege_name_id
 *		Check user privileges on a language given
 *		name usename, language oid, and text priv name.
 */
Datum
has_language_privilege_name_id(PG_FUNCTION_ARGS)
{
	Name		username = PG_GETARG_NAME(0);
	Oid			languageoid = PG_GETARG_OID(1);
	text	   *priv_type_text = PG_GETARG_TEXT_P(2);
	int32		usesysid;
	AclMode		mode;
	AclResult	aclresult;

	usesysid = get_usesysid(NameStr(*username));
	mode = convert_language_priv_string(priv_type_text);

	aclresult = pg_language_aclcheck(languageoid, usesysid, mode);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}

/*
 * has_language_privilege_id
 *		Check user privileges on a language given
 *		language oid, and text priv name.
 *		current_user is assumed
 */
Datum
has_language_privilege_id(PG_FUNCTION_ARGS)
{
	Oid			languageoid = PG_GETARG_OID(0);
	text	   *priv_type_text = PG_GETARG_TEXT_P(1);
	AclId		usesysid;
	AclMode		mode;
	AclResult	aclresult;

	usesysid = GetUserId();
	mode = convert_language_priv_string(priv_type_text);

	aclresult = pg_language_aclcheck(languageoid, usesysid, mode);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}

/*
 * has_language_privilege_id_name
 *		Check user privileges on a language given
 *		usesysid, text languagename, and text priv name.
 */
Datum
has_language_privilege_id_name(PG_FUNCTION_ARGS)
{
	int32		usesysid = PG_GETARG_INT32(0);
	text	   *languagename = PG_GETARG_TEXT_P(1);
	text	   *priv_type_text = PG_GETARG_TEXT_P(2);
	Oid			languageoid;
	AclMode		mode;
	AclResult	aclresult;

	languageoid = convert_language_name(languagename);
	mode = convert_language_priv_string(priv_type_text);

	aclresult = pg_language_aclcheck(languageoid, usesysid, mode);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}

/*
 * has_language_privilege_id_id
 *		Check user privileges on a language given
 *		usesysid, language oid, and text priv name.
 */
Datum
has_language_privilege_id_id(PG_FUNCTION_ARGS)
{
	int32		usesysid = PG_GETARG_INT32(0);
	Oid			languageoid = PG_GETARG_OID(1);
	text	   *priv_type_text = PG_GETARG_TEXT_P(2);
	AclMode		mode;
	AclResult	aclresult;

	mode = convert_language_priv_string(priv_type_text);

	aclresult = pg_language_aclcheck(languageoid, usesysid, mode);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}

/*
 *		Support routines for has_language_privilege family.
 */

/*
 * Given a language name expressed as a string, look it up and return Oid
 */
static Oid
convert_language_name(text *languagename)
{
	char	   *langname;
	Oid			oid;

	langname = DatumGetCString(DirectFunctionCall1(textout,
										 PointerGetDatum(languagename)));

	oid = GetSysCacheOid(LANGNAME,
						 CStringGetDatum(langname),
						 0, 0, 0);
	if (!OidIsValid(oid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("language \"%s\" does not exist", langname)));

	return oid;
}

/*
 * convert_language_priv_string
 *		Convert text string to AclMode value.
 */
static AclMode
convert_language_priv_string(text *priv_type_text)
{
	char	   *priv_type;

	priv_type = DatumGetCString(DirectFunctionCall1(textout,
									   PointerGetDatum(priv_type_text)));

	/*
	 * Return mode from priv_type string
	 */
	if (strcasecmp(priv_type, "USAGE") == 0)
		return ACL_USAGE;
	if (strcasecmp(priv_type, "USAGE WITH GRANT OPTION") == 0)
		return ACL_GRANT_OPTION_FOR(ACL_USAGE);

	ereport(ERROR,
			(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			 errmsg("unrecognized privilege type: \"%s\"", priv_type)));
	return ACL_NO_RIGHTS;		/* keep compiler quiet */
}


/*
 * has_schema_privilege variants
 *		These are all named "has_schema_privilege" at the SQL level.
 *		They take various combinations of schema name, schema OID,
 *		user name, user sysid, or implicit user = current_user.
 *
 *		The result is a boolean value: true if user has the indicated
 *		privilege, false if not.
 */

/*
 * has_schema_privilege_name_name
 *		Check user privileges on a schema given
 *		name username, text schemaname, and text priv name.
 */
Datum
has_schema_privilege_name_name(PG_FUNCTION_ARGS)
{
	Name		username = PG_GETARG_NAME(0);
	text	   *schemaname = PG_GETARG_TEXT_P(1);
	text	   *priv_type_text = PG_GETARG_TEXT_P(2);
	int32		usesysid;
	Oid			schemaoid;
	AclMode		mode;
	AclResult	aclresult;

	usesysid = get_usesysid(NameStr(*username));
	schemaoid = convert_schema_name(schemaname);
	mode = convert_schema_priv_string(priv_type_text);

	aclresult = pg_namespace_aclcheck(schemaoid, usesysid, mode);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}

/*
 * has_schema_privilege_name
 *		Check user privileges on a schema given
 *		text schemaname and text priv name.
 *		current_user is assumed
 */
Datum
has_schema_privilege_name(PG_FUNCTION_ARGS)
{
	text	   *schemaname = PG_GETARG_TEXT_P(0);
	text	   *priv_type_text = PG_GETARG_TEXT_P(1);
	AclId		usesysid;
	Oid			schemaoid;
	AclMode		mode;
	AclResult	aclresult;

	usesysid = GetUserId();
	schemaoid = convert_schema_name(schemaname);
	mode = convert_schema_priv_string(priv_type_text);

	aclresult = pg_namespace_aclcheck(schemaoid, usesysid, mode);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}

/*
 * has_schema_privilege_name_id
 *		Check user privileges on a schema given
 *		name usename, schema oid, and text priv name.
 */
Datum
has_schema_privilege_name_id(PG_FUNCTION_ARGS)
{
	Name		username = PG_GETARG_NAME(0);
	Oid			schemaoid = PG_GETARG_OID(1);
	text	   *priv_type_text = PG_GETARG_TEXT_P(2);
	int32		usesysid;
	AclMode		mode;
	AclResult	aclresult;

	usesysid = get_usesysid(NameStr(*username));
	mode = convert_schema_priv_string(priv_type_text);

	aclresult = pg_namespace_aclcheck(schemaoid, usesysid, mode);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}

/*
 * has_schema_privilege_id
 *		Check user privileges on a schema given
 *		schema oid, and text priv name.
 *		current_user is assumed
 */
Datum
has_schema_privilege_id(PG_FUNCTION_ARGS)
{
	Oid			schemaoid = PG_GETARG_OID(0);
	text	   *priv_type_text = PG_GETARG_TEXT_P(1);
	AclId		usesysid;
	AclMode		mode;
	AclResult	aclresult;

	usesysid = GetUserId();
	mode = convert_schema_priv_string(priv_type_text);

	aclresult = pg_namespace_aclcheck(schemaoid, usesysid, mode);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}

/*
 * has_schema_privilege_id_name
 *		Check user privileges on a schema given
 *		usesysid, text schemaname, and text priv name.
 */
Datum
has_schema_privilege_id_name(PG_FUNCTION_ARGS)
{
	int32		usesysid = PG_GETARG_INT32(0);
	text	   *schemaname = PG_GETARG_TEXT_P(1);
	text	   *priv_type_text = PG_GETARG_TEXT_P(2);
	Oid			schemaoid;
	AclMode		mode;
	AclResult	aclresult;

	schemaoid = convert_schema_name(schemaname);
	mode = convert_schema_priv_string(priv_type_text);

	aclresult = pg_namespace_aclcheck(schemaoid, usesysid, mode);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}

/*
 * has_schema_privilege_id_id
 *		Check user privileges on a schema given
 *		usesysid, schema oid, and text priv name.
 */
Datum
has_schema_privilege_id_id(PG_FUNCTION_ARGS)
{
	int32		usesysid = PG_GETARG_INT32(0);
	Oid			schemaoid = PG_GETARG_OID(1);
	text	   *priv_type_text = PG_GETARG_TEXT_P(2);
	AclMode		mode;
	AclResult	aclresult;

	mode = convert_schema_priv_string(priv_type_text);

	aclresult = pg_namespace_aclcheck(schemaoid, usesysid, mode);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}

/*
 *		Support routines for has_schema_privilege family.
 */

/*
 * Given a schema name expressed as a string, look it up and return Oid
 */
static Oid
convert_schema_name(text *schemaname)
{
	char	   *nspname;
	Oid			oid;

	nspname = DatumGetCString(DirectFunctionCall1(textout,
										   PointerGetDatum(schemaname)));

	oid = GetSysCacheOid(NAMESPACENAME,
						 CStringGetDatum(nspname),
						 0, 0, 0);
	if (!OidIsValid(oid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_SCHEMA),
				 errmsg("schema \"%s\" does not exist", nspname)));

	return oid;
}

/*
 * convert_schema_priv_string
 *		Convert text string to AclMode value.
 */
static AclMode
convert_schema_priv_string(text *priv_type_text)
{
	char	   *priv_type;

	priv_type = DatumGetCString(DirectFunctionCall1(textout,
									   PointerGetDatum(priv_type_text)));

	/*
	 * Return mode from priv_type string
	 */
	if (strcasecmp(priv_type, "CREATE") == 0)
		return ACL_CREATE;
	if (strcasecmp(priv_type, "CREATE WITH GRANT OPTION") == 0)
		return ACL_GRANT_OPTION_FOR(ACL_CREATE);

	if (strcasecmp(priv_type, "USAGE") == 0)
		return ACL_USAGE;
	if (strcasecmp(priv_type, "USAGE WITH GRANT OPTION") == 0)
		return ACL_GRANT_OPTION_FOR(ACL_USAGE);

	ereport(ERROR,
			(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			 errmsg("unrecognized privilege type: \"%s\"", priv_type)));
	return ACL_NO_RIGHTS;		/* keep compiler quiet */
}
