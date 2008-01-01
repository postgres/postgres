/*-------------------------------------------------------------------------
 *
 * acl.c
 *	  Basic access control list data structures manipulation routines.
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/utils/adt/acl.c,v 1.139 2008/01/01 19:45:52 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>

#include "catalog/namespace.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_auth_members.h"
#include "catalog/pg_type.h"
#include "commands/dbcommands.h"
#include "commands/tablespace.h"
#include "miscadmin.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/syscache.h"


/*
 * We frequently need to test whether a given role is a member of some other
 * role.  In most of these tests the "given role" is the same, namely the
 * active current user.  So we can optimize it by keeping a cached list of
 * all the roles the "given role" is a member of, directly or indirectly.
 * The cache is flushed whenever we detect a change in pg_auth_members.
 *
 * There are actually two caches, one computed under "has_privs" rules
 * (do not recurse where rolinherit isn't true) and one computed under
 * "is_member" rules (recurse regardless of rolinherit).
 *
 * Possibly this mechanism should be generalized to allow caching membership
 * info for multiple roles?
 *
 * The has_privs cache is:
 * cached_privs_role is the role OID the cache is for.
 * cached_privs_roles is an OID list of roles that cached_privs_role
 *		has the privileges of (always including itself).
 * The cache is valid if cached_privs_role is not InvalidOid.
 *
 * The is_member cache is similarly:
 * cached_member_role is the role OID the cache is for.
 * cached_membership_roles is an OID list of roles that cached_member_role
 *		is a member of (always including itself).
 * The cache is valid if cached_member_role is not InvalidOid.
 */
static Oid	cached_privs_role = InvalidOid;
static List *cached_privs_roles = NIL;
static Oid	cached_member_role = InvalidOid;
static List *cached_membership_roles = NIL;


static const char *getid(const char *s, char *n);
static void putid(char *p, const char *s);
static Acl *allocacl(int n);
static void check_acl(const Acl *acl);
static const char *aclparse(const char *s, AclItem *aip);
static bool aclitem_match(const AclItem *a1, const AclItem *a2);
static void check_circularity(const Acl *old_acl, const AclItem *mod_aip,
				  Oid ownerId);
static Acl *recursive_revoke(Acl *acl, Oid grantee, AclMode revoke_privs,
				 Oid ownerId, DropBehavior behavior);
static int	oidComparator(const void *arg1, const void *arg2);

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
static Oid	convert_tablespace_name(text *tablespacename);
static AclMode convert_tablespace_priv_string(text *priv_type_text);
static AclMode convert_role_priv_string(text *priv_type_text);
static AclResult pg_role_aclcheck(Oid role_oid, Oid roleid, AclMode mode);

static void RoleMembershipCacheCallback(Datum arg, Oid relid);


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
 * Write a role name at *p, adding double quotes if needed.
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
 *		The group|user decoration is unnecessary in the roles world,
 *		but we still accept it for backward compatibility.
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
	char		name[NAMEDATALEN];
	char		name2[NAMEDATALEN];

	Assert(s && aip);

#ifdef ACLDEBUG
	elog(LOG, "aclparse: input = \"%s\"", s);
#endif
	s = getid(s, name);
	if (*s != '=')
	{
		/* we just read a keyword, not a name */
		if (strcmp(name, "group") != 0 && strcmp(name, "user") != 0)
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
			case ACL_CONNECT_CHR:
				read = ACL_CONNECT;
				break;
			case 'R':			/* ignore old RULE privileges */
				read = 0;
				break;
			default:
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					  errmsg("invalid mode character: must be one of \"%s\"",
							 ACL_ALL_RIGHTS_STR)));
		}

		privs |= read;
	}

	if (name[0] == '\0')
		aip->ai_grantee = ACL_ID_PUBLIC;
	else
		aip->ai_grantee = get_roleid_checked(name);

	/*
	 * XXX Allow a degree of backward compatibility by defaulting the grantor
	 * to the superuser.
	 */
	if (*s == '/')
	{
		s = getid(s + 1, name2);
		if (name2[0] == '\0')
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("a name must follow the \"/\" sign")));
		aip->ai_grantor = get_roleid_checked(name2);
	}
	else
	{
		aip->ai_grantor = BOOTSTRAP_SUPERUSERID;
		ereport(WARNING,
				(errcode(ERRCODE_INVALID_GRANTOR),
				 errmsg("defaulting grantor to user ID %u",
						BOOTSTRAP_SUPERUSERID)));
	}

	ACLITEM_SET_PRIVS_GOPTIONS(*aip, privs, goption);

#ifdef ACLDEBUG
	elog(LOG, "aclparse: correctly read [%u %x %x]",
		 aip->ai_grantee, privs, goption);
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
	SET_VARSIZE(new_acl, size);
	new_acl->ndim = 1;
	new_acl->dataoffset = 0;	/* we never put in any nulls */
	new_acl->elemtype = ACLITEMOID;
	ARR_LBOUND(new_acl)[0] = 1;
	ARR_DIMS(new_acl)[0] = n;
	return new_acl;
}

/*
 * Verify that an ACL array is acceptable (one-dimensional and has no nulls)
 */
static void
check_acl(const Acl *acl)
{
	if (ARR_ELEMTYPE(acl) != ACLITEMOID)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("ACL array contains wrong data type")));
	if (ARR_NDIM(acl) != 1)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("ACL arrays must be one-dimensional")));
	if (ARR_HASNULL(acl))
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("ACL arrays must not contain null values")));
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

	out = palloc(strlen("=/") +
				 2 * N_ACL_RIGHTS +
				 2 * (2 * NAMEDATALEN + 2) +
				 1);

	p = out;
	*p = '\0';

	if (aip->ai_grantee != ACL_ID_PUBLIC)
	{
		htup = SearchSysCache(AUTHOID,
							  ObjectIdGetDatum(aip->ai_grantee),
							  0, 0, 0);
		if (HeapTupleIsValid(htup))
		{
			putid(p, NameStr(((Form_pg_authid) GETSTRUCT(htup))->rolname));
			ReleaseSysCache(htup);
		}
		else
		{
			/* Generate numeric OID if we don't find an entry */
			sprintf(p, "%u", aip->ai_grantee);
		}
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

	htup = SearchSysCache(AUTHOID,
						  ObjectIdGetDatum(aip->ai_grantor),
						  0, 0, 0);
	if (HeapTupleIsValid(htup))
	{
		putid(p, NameStr(((Form_pg_authid) GETSTRUCT(htup))->rolname));
		ReleaseSysCache(htup);
	}
	else
	{
		/* Generate numeric OID if we don't find an entry */
		sprintf(p, "%u", aip->ai_grantor);
	}

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
	return a1->ai_grantee == a2->ai_grantee &&
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
acldefault(GrantObjectType objtype, Oid ownerId)
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
		case ACL_OBJECT_SEQUENCE:
			world_default = ACL_NO_RIGHTS;
			owner_default = ACL_ALL_RIGHTS_SEQUENCE;
			break;
		case ACL_OBJECT_DATABASE:
			/* for backwards compatibility, grant some rights by default */
			world_default = ACL_CREATE_TEMP | ACL_CONNECT;
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
		case ACL_OBJECT_TABLESPACE:
			world_default = ACL_NO_RIGHTS;
			owner_default = ACL_ALL_RIGHTS_TABLESPACE;
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
		aip->ai_grantee = ACL_ID_PUBLIC;
		aip->ai_grantor = ownerId;
		ACLITEM_SET_PRIVS_GOPTIONS(*aip, world_default, ACL_NO_RIGHTS);
		aip++;
	}

	/*
	 * Note that the owner's entry shows all ordinary privileges but no grant
	 * options.  This is because his grant options come "from the system" and
	 * not from his own efforts.  (The SQL spec says that the owner's rights
	 * come from a "_SYSTEM" authid.)  However, we do consider that the
	 * owner's ordinary privileges are self-granted; this lets him revoke
	 * them.  We implement the owner's grant options without any explicit
	 * "_SYSTEM"-like ACL entry, by internally special-casing the owner
	 * whereever we are testing grant options.
	 */
	aip->ai_grantee = ownerId;
	aip->ai_grantor = ownerId;
	ACLITEM_SET_PRIVS_GOPTIONS(*aip, owner_default, ACL_NO_RIGHTS);

	return acl;
}


/*
 * Update an ACL array to add or remove specified privileges.
 *
 *	old_acl: the input ACL array
 *	mod_aip: defines the privileges to be added, removed, or substituted
 *	modechg: ACL_MODECHG_ADD, ACL_MODECHG_DEL, or ACL_MODECHG_EQL
 *	ownerId: Oid of object owner
 *	behavior: RESTRICT or CASCADE behavior for recursive removal
 *
 * ownerid and behavior are only relevant when the update operation specifies
 * deletion of grant options.
 *
 * The result is a modified copy; the input object is not changed.
 *
 * NB: caller is responsible for having detoasted the input ACL, if needed.
 */
Acl *
aclupdate(const Acl *old_acl, const AclItem *mod_aip,
		  int modechg, Oid ownerId, DropBehavior behavior)
{
	Acl		   *new_acl = NULL;
	AclItem    *old_aip,
			   *new_aip = NULL;
	AclMode		old_rights,
				old_goptions,
				new_rights,
				new_goptions;
	int			dst,
				num;

	/* Caller probably already checked old_acl, but be safe */
	check_acl(old_acl);

	/* If granting grant options, check for circularity */
	if (modechg != ACL_MODECHG_DEL &&
		ACLITEM_GET_GOPTIONS(*mod_aip) != ACL_NO_RIGHTS)
		check_circularity(old_acl, mod_aip, ownerId);

	num = ACL_NUM(old_acl);
	old_aip = ACL_DAT(old_acl);

	/*
	 * Search the ACL for an existing entry for this grantee and grantor. If
	 * one exists, just modify the entry in-place (well, in the same position,
	 * since we actually return a copy); otherwise, insert the new entry at
	 * the end.
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
		ACLITEM_SET_PRIVS_GOPTIONS(new_aip[dst],
								   ACL_NO_RIGHTS, ACL_NO_RIGHTS);
		num++;					/* set num to the size of new_acl */
	}

	old_rights = ACLITEM_GET_RIGHTS(new_aip[dst]);
	old_goptions = ACLITEM_GET_GOPTIONS(new_aip[dst]);

	/* apply the specified permissions change */
	switch (modechg)
	{
		case ACL_MODECHG_ADD:
			ACLITEM_SET_RIGHTS(new_aip[dst],
							   old_rights | ACLITEM_GET_RIGHTS(*mod_aip));
			break;
		case ACL_MODECHG_DEL:
			ACLITEM_SET_RIGHTS(new_aip[dst],
							   old_rights & ~ACLITEM_GET_RIGHTS(*mod_aip));
			break;
		case ACL_MODECHG_EQL:
			ACLITEM_SET_RIGHTS(new_aip[dst],
							   ACLITEM_GET_RIGHTS(*mod_aip));
			break;
	}

	new_rights = ACLITEM_GET_RIGHTS(new_aip[dst]);
	new_goptions = ACLITEM_GET_GOPTIONS(new_aip[dst]);

	/*
	 * If the adjusted entry has no permissions, delete it from the list.
	 */
	if (new_rights == ACL_NO_RIGHTS)
	{
		memmove(new_aip + dst,
				new_aip + dst + 1,
				(num - dst - 1) * sizeof(AclItem));
		/* Adjust array size to be 'num - 1' items */
		ARR_DIMS(new_acl)[0] = num - 1;
		SET_VARSIZE(new_acl, ACL_N_SIZE(num - 1));
	}

	/*
	 * Remove abandoned privileges (cascading revoke).	Currently we can only
	 * handle this when the grantee is not PUBLIC.
	 */
	if ((old_goptions & ~new_goptions) != 0)
	{
		Assert(mod_aip->ai_grantee != ACL_ID_PUBLIC);
		new_acl = recursive_revoke(new_acl, mod_aip->ai_grantee,
								   (old_goptions & ~new_goptions),
								   ownerId, behavior);
	}

	return new_acl;
}

/*
 * Update an ACL array to reflect a change of owner to the parent object
 *
 *	old_acl: the input ACL array (must not be NULL)
 *	oldOwnerId: Oid of the old object owner
 *	newOwnerId: Oid of the new object owner
 *
 * The result is a modified copy; the input object is not changed.
 *
 * NB: caller is responsible for having detoasted the input ACL, if needed.
 */
Acl *
aclnewowner(const Acl *old_acl, Oid oldOwnerId, Oid newOwnerId)
{
	Acl		   *new_acl;
	AclItem    *new_aip;
	AclItem    *old_aip;
	AclItem    *dst_aip;
	AclItem    *src_aip;
	AclItem    *targ_aip;
	bool		newpresent = false;
	int			dst,
				src,
				targ,
				num;

	check_acl(old_acl);

	/*
	 * Make a copy of the given ACL, substituting new owner ID for old
	 * wherever it appears as either grantor or grantee.  Also note if the new
	 * owner ID is already present.
	 */
	num = ACL_NUM(old_acl);
	old_aip = ACL_DAT(old_acl);
	new_acl = allocacl(num);
	new_aip = ACL_DAT(new_acl);
	memcpy(new_aip, old_aip, num * sizeof(AclItem));
	for (dst = 0, dst_aip = new_aip; dst < num; dst++, dst_aip++)
	{
		if (dst_aip->ai_grantor == oldOwnerId)
			dst_aip->ai_grantor = newOwnerId;
		else if (dst_aip->ai_grantor == newOwnerId)
			newpresent = true;
		if (dst_aip->ai_grantee == oldOwnerId)
			dst_aip->ai_grantee = newOwnerId;
		else if (dst_aip->ai_grantee == newOwnerId)
			newpresent = true;
	}

	/*
	 * If the old ACL contained any references to the new owner, then we may
	 * now have generated an ACL containing duplicate entries.	Find them and
	 * merge them so that there are not duplicates.  (This is relatively
	 * expensive since we use a stupid O(N^2) algorithm, but it's unlikely to
	 * be the normal case.)
	 *
	 * To simplify deletion of duplicate entries, we temporarily leave them in
	 * the array but set their privilege masks to zero; when we reach such an
	 * entry it's just skipped.  (Thus, a side effect of this code will be to
	 * remove privilege-free entries, should there be any in the input.)  dst
	 * is the next output slot, targ is the currently considered input slot
	 * (always >= dst), and src scans entries to the right of targ looking for
	 * duplicates.	Once an entry has been emitted to dst it is known
	 * duplicate-free and need not be considered anymore.
	 */
	if (newpresent)
	{
		dst = 0;
		for (targ = 0, targ_aip = new_aip; targ < num; targ++, targ_aip++)
		{
			/* ignore if deleted in an earlier pass */
			if (ACLITEM_GET_RIGHTS(*targ_aip) == ACL_NO_RIGHTS)
				continue;
			/* find and merge any duplicates */
			for (src = targ + 1, src_aip = targ_aip + 1; src < num;
				 src++, src_aip++)
			{
				if (ACLITEM_GET_RIGHTS(*src_aip) == ACL_NO_RIGHTS)
					continue;
				if (aclitem_match(targ_aip, src_aip))
				{
					ACLITEM_SET_RIGHTS(*targ_aip,
									   ACLITEM_GET_RIGHTS(*targ_aip) |
									   ACLITEM_GET_RIGHTS(*src_aip));
					/* mark the duplicate deleted */
					ACLITEM_SET_RIGHTS(*src_aip, ACL_NO_RIGHTS);
				}
			}
			/* and emit to output */
			new_aip[dst] = *targ_aip;
			dst++;
		}
		/* Adjust array size to be 'dst' items */
		ARR_DIMS(new_acl)[0] = dst;
		SET_VARSIZE(new_acl, ACL_N_SIZE(dst));
	}

	return new_acl;
}


/*
 * When granting grant options, we must disallow attempts to set up circular
 * chains of grant options.  Suppose A (the object owner) grants B some
 * privileges with grant option, and B re-grants them to C.  If C could
 * grant the privileges to B as well, then A would be unable to effectively
 * revoke the privileges from B, since recursive_revoke would consider that
 * B still has 'em from C.
 *
 * We check for this by recursively deleting all grant options belonging to
 * the target grantee, and then seeing if the would-be grantor still has the
 * grant option or not.
 */
static void
check_circularity(const Acl *old_acl, const AclItem *mod_aip,
				  Oid ownerId)
{
	Acl		   *acl;
	AclItem    *aip;
	int			i,
				num;
	AclMode		own_privs;

	check_acl(old_acl);

	/*
	 * For now, grant options can only be granted to roles, not PUBLIC.
	 * Otherwise we'd have to work a bit harder here.
	 */
	Assert(mod_aip->ai_grantee != ACL_ID_PUBLIC);

	/* The owner always has grant options, no need to check */
	if (mod_aip->ai_grantor == ownerId)
		return;

	/* Make a working copy */
	acl = allocacl(ACL_NUM(old_acl));
	memcpy(acl, old_acl, ACL_SIZE(old_acl));

	/* Zap all grant options of target grantee, plus what depends on 'em */
cc_restart:
	num = ACL_NUM(acl);
	aip = ACL_DAT(acl);
	for (i = 0; i < num; i++)
	{
		if (aip[i].ai_grantee == mod_aip->ai_grantee &&
			ACLITEM_GET_GOPTIONS(aip[i]) != ACL_NO_RIGHTS)
		{
			Acl		   *new_acl;

			/* We'll actually zap ordinary privs too, but no matter */
			new_acl = aclupdate(acl, &aip[i], ACL_MODECHG_DEL,
								ownerId, DROP_CASCADE);

			pfree(acl);
			acl = new_acl;

			goto cc_restart;
		}
	}

	/* Now we can compute grantor's independently-derived privileges */
	own_privs = aclmask(acl,
						mod_aip->ai_grantor,
						ownerId,
						ACL_GRANT_OPTION_FOR(ACLITEM_GET_GOPTIONS(*mod_aip)),
						ACLMASK_ALL);
	own_privs = ACL_OPTION_TO_PRIVS(own_privs);

	if ((ACLITEM_GET_GOPTIONS(*mod_aip) & ~own_privs) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_GRANT_OPERATION),
		errmsg("grant options cannot be granted back to your own grantor")));

	pfree(acl);
}


/*
 * Ensure that no privilege is "abandoned".  A privilege is abandoned
 * if the user that granted the privilege loses the grant option.  (So
 * the chain through which it was granted is broken.)  Either the
 * abandoned privileges are revoked as well, or an error message is
 * printed, depending on the drop behavior option.
 *
 *	acl: the input ACL list
 *	grantee: the user from whom some grant options have been revoked
 *	revoke_privs: the grant options being revoked
 *	ownerId: Oid of object owner
 *	behavior: RESTRICT or CASCADE behavior for recursive removal
 *
 * The input Acl object is pfree'd if replaced.
 */
static Acl *
recursive_revoke(Acl *acl,
				 Oid grantee,
				 AclMode revoke_privs,
				 Oid ownerId,
				 DropBehavior behavior)
{
	AclMode		still_has;
	AclItem    *aip;
	int			i,
				num;

	check_acl(acl);

	/* The owner can never truly lose grant options, so short-circuit */
	if (grantee == ownerId)
		return acl;

	/* The grantee might still have the privileges via another grantor */
	still_has = aclmask(acl, grantee, ownerId,
						ACL_GRANT_OPTION_FOR(revoke_privs),
						ACLMASK_ALL);
	revoke_privs &= ~still_has;
	if (revoke_privs == ACL_NO_RIGHTS)
		return acl;

restart:
	num = ACL_NUM(acl);
	aip = ACL_DAT(acl);
	for (i = 0; i < num; i++)
	{
		if (aip[i].ai_grantor == grantee
			&& (ACLITEM_GET_PRIVS(aip[i]) & revoke_privs) != 0)
		{
			AclItem		mod_acl;
			Acl		   *new_acl;

			if (behavior == DROP_RESTRICT)
				ereport(ERROR,
						(errcode(ERRCODE_DEPENDENT_OBJECTS_STILL_EXIST),
						 errmsg("dependent privileges exist"),
						 errhint("Use CASCADE to revoke them too.")));

			mod_acl.ai_grantor = grantee;
			mod_acl.ai_grantee = aip[i].ai_grantee;
			ACLITEM_SET_PRIVS_GOPTIONS(mod_acl,
									   revoke_privs,
									   revoke_privs);

			new_acl = aclupdate(acl, &mod_acl, ACL_MODECHG_DEL,
								ownerId, behavior);

			pfree(acl);
			acl = new_acl;

			goto restart;
		}
	}

	return acl;
}


/*
 * aclmask --- compute bitmask of all privileges held by roleid.
 *
 * When 'how' = ACLMASK_ALL, this simply returns the privilege bits
 * held by the given roleid according to the given ACL list, ANDed
 * with 'mask'.  (The point of passing 'mask' is to let the routine
 * exit early if all privileges of interest have been found.)
 *
 * When 'how' = ACLMASK_ANY, returns as soon as any bit in the mask
 * is known true.  (This lets us exit soonest in cases where the
 * caller is only going to test for zero or nonzero result.)
 *
 * Usage patterns:
 *
 * To see if any of a set of privileges are held:
 *		if (aclmask(acl, roleid, ownerId, privs, ACLMASK_ANY) != 0)
 *
 * To see if all of a set of privileges are held:
 *		if (aclmask(acl, roleid, ownerId, privs, ACLMASK_ALL) == privs)
 *
 * To determine exactly which of a set of privileges are held:
 *		heldprivs = aclmask(acl, roleid, ownerId, privs, ACLMASK_ALL);
 */
AclMode
aclmask(const Acl *acl, Oid roleid, Oid ownerId,
		AclMode mask, AclMaskHow how)
{
	AclMode		result;
	AclMode		remaining;
	AclItem    *aidat;
	int			i,
				num;

	/*
	 * Null ACL should not happen, since caller should have inserted
	 * appropriate default
	 */
	if (acl == NULL)
		elog(ERROR, "null ACL");

	check_acl(acl);

	/* Quick exit for mask == 0 */
	if (mask == 0)
		return 0;

	result = 0;

	/* Owner always implicitly has all grant options */
	if ((mask & ACLITEM_ALL_GOPTION_BITS) &&
		has_privs_of_role(roleid, ownerId))
	{
		result = mask & ACLITEM_ALL_GOPTION_BITS;
		if ((how == ACLMASK_ALL) ? (result == mask) : (result != 0))
			return result;
	}

	num = ACL_NUM(acl);
	aidat = ACL_DAT(acl);

	/*
	 * Check privileges granted directly to roleid or to public
	 */
	for (i = 0; i < num; i++)
	{
		AclItem    *aidata = &aidat[i];

		if (aidata->ai_grantee == ACL_ID_PUBLIC ||
			aidata->ai_grantee == roleid)
		{
			result |= aidata->ai_privs & mask;
			if ((how == ACLMASK_ALL) ? (result == mask) : (result != 0))
				return result;
		}
	}

	/*
	 * Check privileges granted indirectly via role memberships. We do this in
	 * a separate pass to minimize expensive indirect membership tests.  In
	 * particular, it's worth testing whether a given ACL entry grants any
	 * privileges still of interest before we perform the has_privs_of_role
	 * test.
	 */
	remaining = mask & ~result;
	for (i = 0; i < num; i++)
	{
		AclItem    *aidata = &aidat[i];

		if (aidata->ai_grantee == ACL_ID_PUBLIC ||
			aidata->ai_grantee == roleid)
			continue;			/* already checked it */

		if ((aidata->ai_privs & remaining) &&
			has_privs_of_role(roleid, aidata->ai_grantee))
		{
			result |= aidata->ai_privs & mask;
			if ((how == ACLMASK_ALL) ? (result == mask) : (result != 0))
				return result;
			remaining = mask & ~result;
		}
	}

	return result;
}


/*
 * aclmask_direct --- compute bitmask of all privileges held by roleid.
 *
 * This is exactly like aclmask() except that we consider only privileges
 * held *directly* by roleid, not those inherited via role membership.
 */
static AclMode
aclmask_direct(const Acl *acl, Oid roleid, Oid ownerId,
			   AclMode mask, AclMaskHow how)
{
	AclMode		result;
	AclItem    *aidat;
	int			i,
				num;

	/*
	 * Null ACL should not happen, since caller should have inserted
	 * appropriate default
	 */
	if (acl == NULL)
		elog(ERROR, "null ACL");

	check_acl(acl);

	/* Quick exit for mask == 0 */
	if (mask == 0)
		return 0;

	result = 0;

	/* Owner always implicitly has all grant options */
	if ((mask & ACLITEM_ALL_GOPTION_BITS) &&
		roleid == ownerId)
	{
		result = mask & ACLITEM_ALL_GOPTION_BITS;
		if ((how == ACLMASK_ALL) ? (result == mask) : (result != 0))
			return result;
	}

	num = ACL_NUM(acl);
	aidat = ACL_DAT(acl);

	/*
	 * Check privileges granted directly to roleid (and not to public)
	 */
	for (i = 0; i < num; i++)
	{
		AclItem    *aidata = &aidat[i];

		if (aidata->ai_grantee == roleid)
		{
			result |= aidata->ai_privs & mask;
			if ((how == ACLMASK_ALL) ? (result == mask) : (result != 0))
				return result;
		}
	}

	return result;
}


/*
 * aclmembers
 *		Find out all the roleids mentioned in an Acl.
 *		Note that we do not distinguish grantors from grantees.
 *
 * *roleids is set to point to a palloc'd array containing distinct OIDs
 * in sorted order.  The length of the array is the function result.
 */
int
aclmembers(const Acl *acl, Oid **roleids)
{
	Oid		   *list;
	const AclItem *acldat;
	int			i,
				j,
				k;

	if (acl == NULL || ACL_NUM(acl) == 0)
	{
		*roleids = NULL;
		return 0;
	}

	check_acl(acl);

	/* Allocate the worst-case space requirement */
	list = palloc(ACL_NUM(acl) * 2 * sizeof(Oid));
	acldat = ACL_DAT(acl);

	/*
	 * Walk the ACL collecting mentioned RoleIds.
	 */
	j = 0;
	for (i = 0; i < ACL_NUM(acl); i++)
	{
		const AclItem *ai = &acldat[i];

		if (ai->ai_grantee != ACL_ID_PUBLIC)
			list[j++] = ai->ai_grantee;
		/* grantor is currently never PUBLIC, but let's check anyway */
		if (ai->ai_grantor != ACL_ID_PUBLIC)
			list[j++] = ai->ai_grantor;
	}

	/* Sort the array */
	qsort(list, j, sizeof(Oid), oidComparator);

	/* Remove duplicates from the array */
	k = 0;
	for (i = 1; i < j; i++)
	{
		if (list[k] != list[i])
			list[++k] = list[i];
	}

	/*
	 * We could repalloc the array down to minimum size, but it's hardly worth
	 * it since it's only transient memory.
	 */
	*roleids = list;

	return k + 1;
}

/*
 * oidComparator
 *		qsort comparison function for Oids
 */
static int
oidComparator(const void *arg1, const void *arg2)
{
	Oid			oid1 = *(const Oid *) arg1;
	Oid			oid2 = *(const Oid *) arg2;

	if (oid1 > oid2)
		return 1;
	if (oid1 < oid2)
		return -1;
	return 0;
}


/*
 * aclinsert (exported function)
 */
Datum
aclinsert(PG_FUNCTION_ARGS)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("aclinsert is no longer supported")));

	PG_RETURN_NULL();			/* keep compiler quiet */
}

Datum
aclremove(PG_FUNCTION_ARGS)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("aclremove is no longer supported")));

	PG_RETURN_NULL();			/* keep compiler quiet */
}

Datum
aclcontains(PG_FUNCTION_ARGS)
{
	Acl		   *acl = PG_GETARG_ACL_P(0);
	AclItem    *aip = PG_GETARG_ACLITEM_P(1);
	AclItem    *aidat;
	int			i,
				num;

	check_acl(acl);
	num = ACL_NUM(acl);
	aidat = ACL_DAT(acl);
	for (i = 0; i < num; ++i)
	{
		if (aip->ai_grantee == aidat[i].ai_grantee &&
			aip->ai_grantor == aidat[i].ai_grantor &&
			(ACLITEM_GET_RIGHTS(*aip) & ACLITEM_GET_RIGHTS(aidat[i])) == ACLITEM_GET_RIGHTS(*aip))
			PG_RETURN_BOOL(true);
	}
	PG_RETURN_BOOL(false);
}

Datum
makeaclitem(PG_FUNCTION_ARGS)
{
	Oid			grantee = PG_GETARG_OID(0);
	Oid			grantor = PG_GETARG_OID(1);
	text	   *privtext = PG_GETARG_TEXT_P(2);
	bool		goption = PG_GETARG_BOOL(3);
	AclItem    *result;
	AclMode		priv;

	priv = convert_priv_string(privtext);

	result = (AclItem *) palloc(sizeof(AclItem));

	result->ai_grantee = grantee;
	result->ai_grantor = grantor;

	ACLITEM_SET_PRIVS_GOPTIONS(*result, priv,
							   (goption ? priv : ACL_NO_RIGHTS));

	PG_RETURN_ACLITEM_P(result);
}

static AclMode
convert_priv_string(text *priv_type_text)
{
	char	   *priv_type;

	priv_type = DatumGetCString(DirectFunctionCall1(textout,
										   PointerGetDatum(priv_type_text)));

	if (pg_strcasecmp(priv_type, "SELECT") == 0)
		return ACL_SELECT;
	if (pg_strcasecmp(priv_type, "INSERT") == 0)
		return ACL_INSERT;
	if (pg_strcasecmp(priv_type, "UPDATE") == 0)
		return ACL_UPDATE;
	if (pg_strcasecmp(priv_type, "DELETE") == 0)
		return ACL_DELETE;
	if (pg_strcasecmp(priv_type, "REFERENCES") == 0)
		return ACL_REFERENCES;
	if (pg_strcasecmp(priv_type, "TRIGGER") == 0)
		return ACL_TRIGGER;
	if (pg_strcasecmp(priv_type, "EXECUTE") == 0)
		return ACL_EXECUTE;
	if (pg_strcasecmp(priv_type, "USAGE") == 0)
		return ACL_USAGE;
	if (pg_strcasecmp(priv_type, "CREATE") == 0)
		return ACL_CREATE;
	if (pg_strcasecmp(priv_type, "TEMP") == 0)
		return ACL_CREATE_TEMP;
	if (pg_strcasecmp(priv_type, "TEMPORARY") == 0)
		return ACL_CREATE_TEMP;
	if (pg_strcasecmp(priv_type, "CONNECT") == 0)
		return ACL_CONNECT;
	if (pg_strcasecmp(priv_type, "RULE") == 0)
		return 0;				/* ignore old RULE privileges */

	ereport(ERROR,
			(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			 errmsg("unrecognized privilege type: \"%s\"", priv_type)));
	return ACL_NO_RIGHTS;		/* keep compiler quiet */
}


/*
 * has_table_privilege variants
 *		These are all named "has_table_privilege" at the SQL level.
 *		They take various combinations of relation name, relation OID,
 *		user name, user OID, or implicit user = current_user.
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
	Name		rolename = PG_GETARG_NAME(0);
	text	   *tablename = PG_GETARG_TEXT_P(1);
	text	   *priv_type_text = PG_GETARG_TEXT_P(2);
	Oid			roleid;
	Oid			tableoid;
	AclMode		mode;
	AclResult	aclresult;

	roleid = get_roleid_checked(NameStr(*rolename));
	tableoid = convert_table_name(tablename);
	mode = convert_table_priv_string(priv_type_text);

	aclresult = pg_class_aclcheck(tableoid, roleid, mode);

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
	Oid			roleid;
	Oid			tableoid;
	AclMode		mode;
	AclResult	aclresult;

	roleid = GetUserId();
	tableoid = convert_table_name(tablename);
	mode = convert_table_priv_string(priv_type_text);

	aclresult = pg_class_aclcheck(tableoid, roleid, mode);

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
	Oid			roleid;
	AclMode		mode;
	AclResult	aclresult;

	roleid = get_roleid_checked(NameStr(*username));
	mode = convert_table_priv_string(priv_type_text);

	aclresult = pg_class_aclcheck(tableoid, roleid, mode);

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
	Oid			roleid;
	AclMode		mode;
	AclResult	aclresult;

	roleid = GetUserId();
	mode = convert_table_priv_string(priv_type_text);

	aclresult = pg_class_aclcheck(tableoid, roleid, mode);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}

/*
 * has_table_privilege_id_name
 *		Check user privileges on a table given
 *		roleid, text tablename, and text priv name.
 */
Datum
has_table_privilege_id_name(PG_FUNCTION_ARGS)
{
	Oid			roleid = PG_GETARG_OID(0);
	text	   *tablename = PG_GETARG_TEXT_P(1);
	text	   *priv_type_text = PG_GETARG_TEXT_P(2);
	Oid			tableoid;
	AclMode		mode;
	AclResult	aclresult;

	tableoid = convert_table_name(tablename);
	mode = convert_table_priv_string(priv_type_text);

	aclresult = pg_class_aclcheck(tableoid, roleid, mode);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}

/*
 * has_table_privilege_id_id
 *		Check user privileges on a table given
 *		roleid, table oid, and text priv name.
 */
Datum
has_table_privilege_id_id(PG_FUNCTION_ARGS)
{
	Oid			roleid = PG_GETARG_OID(0);
	Oid			tableoid = PG_GETARG_OID(1);
	text	   *priv_type_text = PG_GETARG_TEXT_P(2);
	AclMode		mode;
	AclResult	aclresult;

	mode = convert_table_priv_string(priv_type_text);

	aclresult = pg_class_aclcheck(tableoid, roleid, mode);

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

	relrv = makeRangeVarFromNameList(textToQualifiedNameList(tablename));

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
	if (pg_strcasecmp(priv_type, "SELECT") == 0)
		return ACL_SELECT;
	if (pg_strcasecmp(priv_type, "SELECT WITH GRANT OPTION") == 0)
		return ACL_GRANT_OPTION_FOR(ACL_SELECT);

	if (pg_strcasecmp(priv_type, "INSERT") == 0)
		return ACL_INSERT;
	if (pg_strcasecmp(priv_type, "INSERT WITH GRANT OPTION") == 0)
		return ACL_GRANT_OPTION_FOR(ACL_INSERT);

	if (pg_strcasecmp(priv_type, "UPDATE") == 0)
		return ACL_UPDATE;
	if (pg_strcasecmp(priv_type, "UPDATE WITH GRANT OPTION") == 0)
		return ACL_GRANT_OPTION_FOR(ACL_UPDATE);

	if (pg_strcasecmp(priv_type, "DELETE") == 0)
		return ACL_DELETE;
	if (pg_strcasecmp(priv_type, "DELETE WITH GRANT OPTION") == 0)
		return ACL_GRANT_OPTION_FOR(ACL_DELETE);

	if (pg_strcasecmp(priv_type, "REFERENCES") == 0)
		return ACL_REFERENCES;
	if (pg_strcasecmp(priv_type, "REFERENCES WITH GRANT OPTION") == 0)
		return ACL_GRANT_OPTION_FOR(ACL_REFERENCES);

	if (pg_strcasecmp(priv_type, "TRIGGER") == 0)
		return ACL_TRIGGER;
	if (pg_strcasecmp(priv_type, "TRIGGER WITH GRANT OPTION") == 0)
		return ACL_GRANT_OPTION_FOR(ACL_TRIGGER);

	if (pg_strcasecmp(priv_type, "RULE") == 0)
		return 0;				/* ignore old RULE privileges */
	if (pg_strcasecmp(priv_type, "RULE WITH GRANT OPTION") == 0)
		return 0;

	ereport(ERROR,
			(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			 errmsg("unrecognized privilege type: \"%s\"", priv_type)));
	return ACL_NO_RIGHTS;		/* keep compiler quiet */
}


/*
 * has_database_privilege variants
 *		These are all named "has_database_privilege" at the SQL level.
 *		They take various combinations of database name, database OID,
 *		user name, user OID, or implicit user = current_user.
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
	Oid			roleid;
	Oid			databaseoid;
	AclMode		mode;
	AclResult	aclresult;

	roleid = get_roleid_checked(NameStr(*username));
	databaseoid = convert_database_name(databasename);
	mode = convert_database_priv_string(priv_type_text);

	aclresult = pg_database_aclcheck(databaseoid, roleid, mode);

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
	Oid			roleid;
	Oid			databaseoid;
	AclMode		mode;
	AclResult	aclresult;

	roleid = GetUserId();
	databaseoid = convert_database_name(databasename);
	mode = convert_database_priv_string(priv_type_text);

	aclresult = pg_database_aclcheck(databaseoid, roleid, mode);

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
	Oid			roleid;
	AclMode		mode;
	AclResult	aclresult;

	roleid = get_roleid_checked(NameStr(*username));
	mode = convert_database_priv_string(priv_type_text);

	aclresult = pg_database_aclcheck(databaseoid, roleid, mode);

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
	Oid			roleid;
	AclMode		mode;
	AclResult	aclresult;

	roleid = GetUserId();
	mode = convert_database_priv_string(priv_type_text);

	aclresult = pg_database_aclcheck(databaseoid, roleid, mode);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}

/*
 * has_database_privilege_id_name
 *		Check user privileges on a database given
 *		roleid, text databasename, and text priv name.
 */
Datum
has_database_privilege_id_name(PG_FUNCTION_ARGS)
{
	Oid			roleid = PG_GETARG_OID(0);
	text	   *databasename = PG_GETARG_TEXT_P(1);
	text	   *priv_type_text = PG_GETARG_TEXT_P(2);
	Oid			databaseoid;
	AclMode		mode;
	AclResult	aclresult;

	databaseoid = convert_database_name(databasename);
	mode = convert_database_priv_string(priv_type_text);

	aclresult = pg_database_aclcheck(databaseoid, roleid, mode);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}

/*
 * has_database_privilege_id_id
 *		Check user privileges on a database given
 *		roleid, database oid, and text priv name.
 */
Datum
has_database_privilege_id_id(PG_FUNCTION_ARGS)
{
	Oid			roleid = PG_GETARG_OID(0);
	Oid			databaseoid = PG_GETARG_OID(1);
	text	   *priv_type_text = PG_GETARG_TEXT_P(2);
	AclMode		mode;
	AclResult	aclresult;

	mode = convert_database_priv_string(priv_type_text);

	aclresult = pg_database_aclcheck(databaseoid, roleid, mode);

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
	if (pg_strcasecmp(priv_type, "CREATE") == 0)
		return ACL_CREATE;
	if (pg_strcasecmp(priv_type, "CREATE WITH GRANT OPTION") == 0)
		return ACL_GRANT_OPTION_FOR(ACL_CREATE);

	if (pg_strcasecmp(priv_type, "TEMPORARY") == 0)
		return ACL_CREATE_TEMP;
	if (pg_strcasecmp(priv_type, "TEMPORARY WITH GRANT OPTION") == 0)
		return ACL_GRANT_OPTION_FOR(ACL_CREATE_TEMP);

	if (pg_strcasecmp(priv_type, "TEMP") == 0)
		return ACL_CREATE_TEMP;
	if (pg_strcasecmp(priv_type, "TEMP WITH GRANT OPTION") == 0)
		return ACL_GRANT_OPTION_FOR(ACL_CREATE_TEMP);

	if (pg_strcasecmp(priv_type, "CONNECT") == 0)
		return ACL_CONNECT;
	if (pg_strcasecmp(priv_type, "CONNECT WITH GRANT OPTION") == 0)
		return ACL_GRANT_OPTION_FOR(ACL_CONNECT);

	ereport(ERROR,
			(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			 errmsg("unrecognized privilege type: \"%s\"", priv_type)));
	return ACL_NO_RIGHTS;		/* keep compiler quiet */
}


/*
 * has_function_privilege variants
 *		These are all named "has_function_privilege" at the SQL level.
 *		They take various combinations of function name, function OID,
 *		user name, user OID, or implicit user = current_user.
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
	Oid			roleid;
	Oid			functionoid;
	AclMode		mode;
	AclResult	aclresult;

	roleid = get_roleid_checked(NameStr(*username));
	functionoid = convert_function_name(functionname);
	mode = convert_function_priv_string(priv_type_text);

	aclresult = pg_proc_aclcheck(functionoid, roleid, mode);

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
	Oid			roleid;
	Oid			functionoid;
	AclMode		mode;
	AclResult	aclresult;

	roleid = GetUserId();
	functionoid = convert_function_name(functionname);
	mode = convert_function_priv_string(priv_type_text);

	aclresult = pg_proc_aclcheck(functionoid, roleid, mode);

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
	Oid			roleid;
	AclMode		mode;
	AclResult	aclresult;

	roleid = get_roleid_checked(NameStr(*username));
	mode = convert_function_priv_string(priv_type_text);

	aclresult = pg_proc_aclcheck(functionoid, roleid, mode);

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
	Oid			roleid;
	AclMode		mode;
	AclResult	aclresult;

	roleid = GetUserId();
	mode = convert_function_priv_string(priv_type_text);

	aclresult = pg_proc_aclcheck(functionoid, roleid, mode);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}

/*
 * has_function_privilege_id_name
 *		Check user privileges on a function given
 *		roleid, text functionname, and text priv name.
 */
Datum
has_function_privilege_id_name(PG_FUNCTION_ARGS)
{
	Oid			roleid = PG_GETARG_OID(0);
	text	   *functionname = PG_GETARG_TEXT_P(1);
	text	   *priv_type_text = PG_GETARG_TEXT_P(2);
	Oid			functionoid;
	AclMode		mode;
	AclResult	aclresult;

	functionoid = convert_function_name(functionname);
	mode = convert_function_priv_string(priv_type_text);

	aclresult = pg_proc_aclcheck(functionoid, roleid, mode);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}

/*
 * has_function_privilege_id_id
 *		Check user privileges on a function given
 *		roleid, function oid, and text priv name.
 */
Datum
has_function_privilege_id_id(PG_FUNCTION_ARGS)
{
	Oid			roleid = PG_GETARG_OID(0);
	Oid			functionoid = PG_GETARG_OID(1);
	text	   *priv_type_text = PG_GETARG_TEXT_P(2);
	AclMode		mode;
	AclResult	aclresult;

	mode = convert_function_priv_string(priv_type_text);

	aclresult = pg_proc_aclcheck(functionoid, roleid, mode);

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
	if (pg_strcasecmp(priv_type, "EXECUTE") == 0)
		return ACL_EXECUTE;
	if (pg_strcasecmp(priv_type, "EXECUTE WITH GRANT OPTION") == 0)
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
 *		user name, user OID, or implicit user = current_user.
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
	Oid			roleid;
	Oid			languageoid;
	AclMode		mode;
	AclResult	aclresult;

	roleid = get_roleid_checked(NameStr(*username));
	languageoid = convert_language_name(languagename);
	mode = convert_language_priv_string(priv_type_text);

	aclresult = pg_language_aclcheck(languageoid, roleid, mode);

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
	Oid			roleid;
	Oid			languageoid;
	AclMode		mode;
	AclResult	aclresult;

	roleid = GetUserId();
	languageoid = convert_language_name(languagename);
	mode = convert_language_priv_string(priv_type_text);

	aclresult = pg_language_aclcheck(languageoid, roleid, mode);

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
	Oid			roleid;
	AclMode		mode;
	AclResult	aclresult;

	roleid = get_roleid_checked(NameStr(*username));
	mode = convert_language_priv_string(priv_type_text);

	aclresult = pg_language_aclcheck(languageoid, roleid, mode);

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
	Oid			roleid;
	AclMode		mode;
	AclResult	aclresult;

	roleid = GetUserId();
	mode = convert_language_priv_string(priv_type_text);

	aclresult = pg_language_aclcheck(languageoid, roleid, mode);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}

/*
 * has_language_privilege_id_name
 *		Check user privileges on a language given
 *		roleid, text languagename, and text priv name.
 */
Datum
has_language_privilege_id_name(PG_FUNCTION_ARGS)
{
	Oid			roleid = PG_GETARG_OID(0);
	text	   *languagename = PG_GETARG_TEXT_P(1);
	text	   *priv_type_text = PG_GETARG_TEXT_P(2);
	Oid			languageoid;
	AclMode		mode;
	AclResult	aclresult;

	languageoid = convert_language_name(languagename);
	mode = convert_language_priv_string(priv_type_text);

	aclresult = pg_language_aclcheck(languageoid, roleid, mode);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}

/*
 * has_language_privilege_id_id
 *		Check user privileges on a language given
 *		roleid, language oid, and text priv name.
 */
Datum
has_language_privilege_id_id(PG_FUNCTION_ARGS)
{
	Oid			roleid = PG_GETARG_OID(0);
	Oid			languageoid = PG_GETARG_OID(1);
	text	   *priv_type_text = PG_GETARG_TEXT_P(2);
	AclMode		mode;
	AclResult	aclresult;

	mode = convert_language_priv_string(priv_type_text);

	aclresult = pg_language_aclcheck(languageoid, roleid, mode);

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
	if (pg_strcasecmp(priv_type, "USAGE") == 0)
		return ACL_USAGE;
	if (pg_strcasecmp(priv_type, "USAGE WITH GRANT OPTION") == 0)
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
 *		user name, user OID, or implicit user = current_user.
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
	Oid			roleid;
	Oid			schemaoid;
	AclMode		mode;
	AclResult	aclresult;

	roleid = get_roleid_checked(NameStr(*username));
	schemaoid = convert_schema_name(schemaname);
	mode = convert_schema_priv_string(priv_type_text);

	aclresult = pg_namespace_aclcheck(schemaoid, roleid, mode);

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
	Oid			roleid;
	Oid			schemaoid;
	AclMode		mode;
	AclResult	aclresult;

	roleid = GetUserId();
	schemaoid = convert_schema_name(schemaname);
	mode = convert_schema_priv_string(priv_type_text);

	aclresult = pg_namespace_aclcheck(schemaoid, roleid, mode);

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
	Oid			roleid;
	AclMode		mode;
	AclResult	aclresult;

	roleid = get_roleid_checked(NameStr(*username));
	mode = convert_schema_priv_string(priv_type_text);

	aclresult = pg_namespace_aclcheck(schemaoid, roleid, mode);

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
	Oid			roleid;
	AclMode		mode;
	AclResult	aclresult;

	roleid = GetUserId();
	mode = convert_schema_priv_string(priv_type_text);

	aclresult = pg_namespace_aclcheck(schemaoid, roleid, mode);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}

/*
 * has_schema_privilege_id_name
 *		Check user privileges on a schema given
 *		roleid, text schemaname, and text priv name.
 */
Datum
has_schema_privilege_id_name(PG_FUNCTION_ARGS)
{
	Oid			roleid = PG_GETARG_OID(0);
	text	   *schemaname = PG_GETARG_TEXT_P(1);
	text	   *priv_type_text = PG_GETARG_TEXT_P(2);
	Oid			schemaoid;
	AclMode		mode;
	AclResult	aclresult;

	schemaoid = convert_schema_name(schemaname);
	mode = convert_schema_priv_string(priv_type_text);

	aclresult = pg_namespace_aclcheck(schemaoid, roleid, mode);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}

/*
 * has_schema_privilege_id_id
 *		Check user privileges on a schema given
 *		roleid, schema oid, and text priv name.
 */
Datum
has_schema_privilege_id_id(PG_FUNCTION_ARGS)
{
	Oid			roleid = PG_GETARG_OID(0);
	Oid			schemaoid = PG_GETARG_OID(1);
	text	   *priv_type_text = PG_GETARG_TEXT_P(2);
	AclMode		mode;
	AclResult	aclresult;

	mode = convert_schema_priv_string(priv_type_text);

	aclresult = pg_namespace_aclcheck(schemaoid, roleid, mode);

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
	if (pg_strcasecmp(priv_type, "CREATE") == 0)
		return ACL_CREATE;
	if (pg_strcasecmp(priv_type, "CREATE WITH GRANT OPTION") == 0)
		return ACL_GRANT_OPTION_FOR(ACL_CREATE);

	if (pg_strcasecmp(priv_type, "USAGE") == 0)
		return ACL_USAGE;
	if (pg_strcasecmp(priv_type, "USAGE WITH GRANT OPTION") == 0)
		return ACL_GRANT_OPTION_FOR(ACL_USAGE);

	ereport(ERROR,
			(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			 errmsg("unrecognized privilege type: \"%s\"", priv_type)));
	return ACL_NO_RIGHTS;		/* keep compiler quiet */
}

/*
 * has_tablespace_privilege variants
 *		These are all named "has_tablespace_privilege" at the SQL level.
 *		They take various combinations of tablespace name, tablespace OID,
 *		user name, user OID, or implicit user = current_user.
 *
 *		The result is a boolean value: true if user has the indicated
 *		privilege, false if not.
 */

/*
 * has_tablespace_privilege_name_name
 *		Check user privileges on a tablespace given
 *		name username, text tablespacename, and text priv name.
 */
Datum
has_tablespace_privilege_name_name(PG_FUNCTION_ARGS)
{
	Name		username = PG_GETARG_NAME(0);
	text	   *tablespacename = PG_GETARG_TEXT_P(1);
	text	   *priv_type_text = PG_GETARG_TEXT_P(2);
	Oid			roleid;
	Oid			tablespaceoid;
	AclMode		mode;
	AclResult	aclresult;

	roleid = get_roleid_checked(NameStr(*username));
	tablespaceoid = convert_tablespace_name(tablespacename);
	mode = convert_tablespace_priv_string(priv_type_text);

	aclresult = pg_tablespace_aclcheck(tablespaceoid, roleid, mode);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}

/*
 * has_tablespace_privilege_name
 *		Check user privileges on a tablespace given
 *		text tablespacename and text priv name.
 *		current_user is assumed
 */
Datum
has_tablespace_privilege_name(PG_FUNCTION_ARGS)
{
	text	   *tablespacename = PG_GETARG_TEXT_P(0);
	text	   *priv_type_text = PG_GETARG_TEXT_P(1);
	Oid			roleid;
	Oid			tablespaceoid;
	AclMode		mode;
	AclResult	aclresult;

	roleid = GetUserId();
	tablespaceoid = convert_tablespace_name(tablespacename);
	mode = convert_tablespace_priv_string(priv_type_text);

	aclresult = pg_tablespace_aclcheck(tablespaceoid, roleid, mode);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}

/*
 * has_tablespace_privilege_name_id
 *		Check user privileges on a tablespace given
 *		name usename, tablespace oid, and text priv name.
 */
Datum
has_tablespace_privilege_name_id(PG_FUNCTION_ARGS)
{
	Name		username = PG_GETARG_NAME(0);
	Oid			tablespaceoid = PG_GETARG_OID(1);
	text	   *priv_type_text = PG_GETARG_TEXT_P(2);
	Oid			roleid;
	AclMode		mode;
	AclResult	aclresult;

	roleid = get_roleid_checked(NameStr(*username));
	mode = convert_tablespace_priv_string(priv_type_text);

	aclresult = pg_tablespace_aclcheck(tablespaceoid, roleid, mode);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}

/*
 * has_tablespace_privilege_id
 *		Check user privileges on a tablespace given
 *		tablespace oid, and text priv name.
 *		current_user is assumed
 */
Datum
has_tablespace_privilege_id(PG_FUNCTION_ARGS)
{
	Oid			tablespaceoid = PG_GETARG_OID(0);
	text	   *priv_type_text = PG_GETARG_TEXT_P(1);
	Oid			roleid;
	AclMode		mode;
	AclResult	aclresult;

	roleid = GetUserId();
	mode = convert_tablespace_priv_string(priv_type_text);

	aclresult = pg_tablespace_aclcheck(tablespaceoid, roleid, mode);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}

/*
 * has_tablespace_privilege_id_name
 *		Check user privileges on a tablespace given
 *		roleid, text tablespacename, and text priv name.
 */
Datum
has_tablespace_privilege_id_name(PG_FUNCTION_ARGS)
{
	Oid			roleid = PG_GETARG_OID(0);
	text	   *tablespacename = PG_GETARG_TEXT_P(1);
	text	   *priv_type_text = PG_GETARG_TEXT_P(2);
	Oid			tablespaceoid;
	AclMode		mode;
	AclResult	aclresult;

	tablespaceoid = convert_tablespace_name(tablespacename);
	mode = convert_tablespace_priv_string(priv_type_text);

	aclresult = pg_tablespace_aclcheck(tablespaceoid, roleid, mode);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}

/*
 * has_tablespace_privilege_id_id
 *		Check user privileges on a tablespace given
 *		roleid, tablespace oid, and text priv name.
 */
Datum
has_tablespace_privilege_id_id(PG_FUNCTION_ARGS)
{
	Oid			roleid = PG_GETARG_OID(0);
	Oid			tablespaceoid = PG_GETARG_OID(1);
	text	   *priv_type_text = PG_GETARG_TEXT_P(2);
	AclMode		mode;
	AclResult	aclresult;

	mode = convert_tablespace_priv_string(priv_type_text);

	aclresult = pg_tablespace_aclcheck(tablespaceoid, roleid, mode);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}

/*
 *		Support routines for has_tablespace_privilege family.
 */

/*
 * Given a tablespace name expressed as a string, look it up and return Oid
 */
static Oid
convert_tablespace_name(text *tablespacename)
{
	char	   *spcname;
	Oid			oid;

	spcname = DatumGetCString(DirectFunctionCall1(textout,
										   PointerGetDatum(tablespacename)));
	oid = get_tablespace_oid(spcname);

	if (!OidIsValid(oid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("tablespace \"%s\" does not exist", spcname)));

	return oid;
}

/*
 * convert_tablespace_priv_string
 *		Convert text string to AclMode value.
 */
static AclMode
convert_tablespace_priv_string(text *priv_type_text)
{
	char	   *priv_type;

	priv_type = DatumGetCString(DirectFunctionCall1(textout,
										   PointerGetDatum(priv_type_text)));

	/*
	 * Return mode from priv_type string
	 */
	if (pg_strcasecmp(priv_type, "CREATE") == 0)
		return ACL_CREATE;
	if (pg_strcasecmp(priv_type, "CREATE WITH GRANT OPTION") == 0)
		return ACL_GRANT_OPTION_FOR(ACL_CREATE);

	ereport(ERROR,
			(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			 errmsg("unrecognized privilege type: \"%s\"", priv_type)));
	return ACL_NO_RIGHTS;		/* keep compiler quiet */
}

/*
 * pg_has_role variants
 *		These are all named "pg_has_role" at the SQL level.
 *		They take various combinations of role name, role OID,
 *		user name, user OID, or implicit user = current_user.
 *
 *		The result is a boolean value: true if user has the indicated
 *		privilege, false if not.
 */

/*
 * pg_has_role_name_name
 *		Check user privileges on a role given
 *		name username, name rolename, and text priv name.
 */
Datum
pg_has_role_name_name(PG_FUNCTION_ARGS)
{
	Name		username = PG_GETARG_NAME(0);
	Name		rolename = PG_GETARG_NAME(1);
	text	   *priv_type_text = PG_GETARG_TEXT_P(2);
	Oid			roleid;
	Oid			roleoid;
	AclMode		mode;
	AclResult	aclresult;

	roleid = get_roleid_checked(NameStr(*username));
	roleoid = get_roleid_checked(NameStr(*rolename));
	mode = convert_role_priv_string(priv_type_text);

	aclresult = pg_role_aclcheck(roleoid, roleid, mode);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}

/*
 * pg_has_role_name
 *		Check user privileges on a role given
 *		name rolename and text priv name.
 *		current_user is assumed
 */
Datum
pg_has_role_name(PG_FUNCTION_ARGS)
{
	Name		rolename = PG_GETARG_NAME(0);
	text	   *priv_type_text = PG_GETARG_TEXT_P(1);
	Oid			roleid;
	Oid			roleoid;
	AclMode		mode;
	AclResult	aclresult;

	roleid = GetUserId();
	roleoid = get_roleid_checked(NameStr(*rolename));
	mode = convert_role_priv_string(priv_type_text);

	aclresult = pg_role_aclcheck(roleoid, roleid, mode);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}

/*
 * pg_has_role_name_id
 *		Check user privileges on a role given
 *		name usename, role oid, and text priv name.
 */
Datum
pg_has_role_name_id(PG_FUNCTION_ARGS)
{
	Name		username = PG_GETARG_NAME(0);
	Oid			roleoid = PG_GETARG_OID(1);
	text	   *priv_type_text = PG_GETARG_TEXT_P(2);
	Oid			roleid;
	AclMode		mode;
	AclResult	aclresult;

	roleid = get_roleid_checked(NameStr(*username));
	mode = convert_role_priv_string(priv_type_text);

	aclresult = pg_role_aclcheck(roleoid, roleid, mode);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}

/*
 * pg_has_role_id
 *		Check user privileges on a role given
 *		role oid, and text priv name.
 *		current_user is assumed
 */
Datum
pg_has_role_id(PG_FUNCTION_ARGS)
{
	Oid			roleoid = PG_GETARG_OID(0);
	text	   *priv_type_text = PG_GETARG_TEXT_P(1);
	Oid			roleid;
	AclMode		mode;
	AclResult	aclresult;

	roleid = GetUserId();
	mode = convert_role_priv_string(priv_type_text);

	aclresult = pg_role_aclcheck(roleoid, roleid, mode);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}

/*
 * pg_has_role_id_name
 *		Check user privileges on a role given
 *		roleid, name rolename, and text priv name.
 */
Datum
pg_has_role_id_name(PG_FUNCTION_ARGS)
{
	Oid			roleid = PG_GETARG_OID(0);
	Name		rolename = PG_GETARG_NAME(1);
	text	   *priv_type_text = PG_GETARG_TEXT_P(2);
	Oid			roleoid;
	AclMode		mode;
	AclResult	aclresult;

	roleoid = get_roleid_checked(NameStr(*rolename));
	mode = convert_role_priv_string(priv_type_text);

	aclresult = pg_role_aclcheck(roleoid, roleid, mode);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}

/*
 * pg_has_role_id_id
 *		Check user privileges on a role given
 *		roleid, role oid, and text priv name.
 */
Datum
pg_has_role_id_id(PG_FUNCTION_ARGS)
{
	Oid			roleid = PG_GETARG_OID(0);
	Oid			roleoid = PG_GETARG_OID(1);
	text	   *priv_type_text = PG_GETARG_TEXT_P(2);
	AclMode		mode;
	AclResult	aclresult;

	mode = convert_role_priv_string(priv_type_text);

	aclresult = pg_role_aclcheck(roleoid, roleid, mode);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}

/*
 *		Support routines for pg_has_role family.
 */

/*
 * convert_role_priv_string
 *		Convert text string to AclMode value.
 *
 * We use USAGE to denote whether the privileges of the role are accessible
 * (has_privs), MEMBER to denote is_member, and MEMBER WITH GRANT OPTION
 * (or ADMIN OPTION) to denote is_admin.  There is no ACL bit corresponding
 * to MEMBER so we cheat and use ACL_CREATE for that.  This convention
 * is shared only with pg_role_aclcheck, below.
 */
static AclMode
convert_role_priv_string(text *priv_type_text)
{
	char	   *priv_type;

	priv_type = DatumGetCString(DirectFunctionCall1(textout,
										   PointerGetDatum(priv_type_text)));

	/*
	 * Return mode from priv_type string
	 */
	if (pg_strcasecmp(priv_type, "USAGE") == 0)
		return ACL_USAGE;
	if (pg_strcasecmp(priv_type, "MEMBER") == 0)
		return ACL_CREATE;
	if (pg_strcasecmp(priv_type, "USAGE WITH GRANT OPTION") == 0 ||
		pg_strcasecmp(priv_type, "USAGE WITH ADMIN OPTION") == 0 ||
		pg_strcasecmp(priv_type, "MEMBER WITH GRANT OPTION") == 0 ||
		pg_strcasecmp(priv_type, "MEMBER WITH ADMIN OPTION") == 0)
		return ACL_GRANT_OPTION_FOR(ACL_CREATE);

	ereport(ERROR,
			(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			 errmsg("unrecognized privilege type: \"%s\"", priv_type)));
	return ACL_NO_RIGHTS;		/* keep compiler quiet */
}

/*
 * pg_role_aclcheck
 *		Quick-and-dirty support for pg_has_role
 */
static AclResult
pg_role_aclcheck(Oid role_oid, Oid roleid, AclMode mode)
{
	if (mode & ACL_GRANT_OPTION_FOR(ACL_CREATE))
	{
		if (is_admin_of_role(roleid, role_oid))
			return ACLCHECK_OK;
	}
	if (mode & ACL_CREATE)
	{
		if (is_member_of_role(roleid, role_oid))
			return ACLCHECK_OK;
	}
	if (mode & ACL_USAGE)
	{
		if (has_privs_of_role(roleid, role_oid))
			return ACLCHECK_OK;
	}
	return ACLCHECK_NO_PRIV;
}


/*
 * initialization function (called by InitPostgres)
 */
void
initialize_acl(void)
{
	if (!IsBootstrapProcessingMode())
	{
		/*
		 * In normal mode, set a callback on any syscache invalidation of
		 * pg_auth_members rows
		 */
		CacheRegisterSyscacheCallback(AUTHMEMROLEMEM,
									  RoleMembershipCacheCallback,
									  (Datum) 0);
	}
}

/*
 * RoleMembershipCacheCallback
 *		Syscache inval callback function
 */
static void
RoleMembershipCacheCallback(Datum arg, Oid relid)
{
	/* Force membership caches to be recomputed on next use */
	cached_privs_role = InvalidOid;
	cached_member_role = InvalidOid;
}


/* Check if specified role has rolinherit set */
static bool
has_rolinherit(Oid roleid)
{
	bool		result = false;
	HeapTuple	utup;

	utup = SearchSysCache(AUTHOID,
						  ObjectIdGetDatum(roleid),
						  0, 0, 0);
	if (HeapTupleIsValid(utup))
	{
		result = ((Form_pg_authid) GETSTRUCT(utup))->rolinherit;
		ReleaseSysCache(utup);
	}
	return result;
}


/*
 * Get a list of roles that the specified roleid has the privileges of
 *
 * This is defined not to recurse through roles that don't have rolinherit
 * set; for such roles, membership implies the ability to do SET ROLE, but
 * the privileges are not available until you've done so.
 *
 * Since indirect membership testing is relatively expensive, we cache
 * a list of memberships.  Hence, the result is only guaranteed good until
 * the next call of roles_has_privs_of()!
 *
 * For the benefit of select_best_grantor, the result is defined to be
 * in breadth-first order, ie, closer relationships earlier.
 */
static List *
roles_has_privs_of(Oid roleid)
{
	List	   *roles_list;
	ListCell   *l;
	List	   *new_cached_privs_roles;
	MemoryContext oldctx;

	/* If cache is already valid, just return the list */
	if (OidIsValid(cached_privs_role) && cached_privs_role == roleid)
		return cached_privs_roles;

	/*
	 * Find all the roles that roleid is a member of, including multi-level
	 * recursion.  The role itself will always be the first element of the
	 * resulting list.
	 *
	 * Each element of the list is scanned to see if it adds any indirect
	 * memberships.  We can use a single list as both the record of
	 * already-found memberships and the agenda of roles yet to be scanned.
	 * This is a bit tricky but works because the foreach() macro doesn't
	 * fetch the next list element until the bottom of the loop.
	 */
	roles_list = list_make1_oid(roleid);

	foreach(l, roles_list)
	{
		Oid			memberid = lfirst_oid(l);
		CatCList   *memlist;
		int			i;

		/* Ignore non-inheriting roles */
		if (!has_rolinherit(memberid))
			continue;

		/* Find roles that memberid is directly a member of */
		memlist = SearchSysCacheList(AUTHMEMMEMROLE, 1,
									 ObjectIdGetDatum(memberid),
									 0, 0, 0);
		for (i = 0; i < memlist->n_members; i++)
		{
			HeapTuple	tup = &memlist->members[i]->tuple;
			Oid			otherid = ((Form_pg_auth_members) GETSTRUCT(tup))->roleid;

			/*
			 * Even though there shouldn't be any loops in the membership
			 * graph, we must test for having already seen this role. It is
			 * legal for instance to have both A->B and A->C->B.
			 */
			roles_list = list_append_unique_oid(roles_list, otherid);
		}
		ReleaseSysCacheList(memlist);
	}

	/*
	 * Copy the completed list into TopMemoryContext so it will persist.
	 */
	oldctx = MemoryContextSwitchTo(TopMemoryContext);
	new_cached_privs_roles = list_copy(roles_list);
	MemoryContextSwitchTo(oldctx);
	list_free(roles_list);

	/*
	 * Now safe to assign to state variable
	 */
	cached_privs_role = InvalidOid;		/* just paranoia */
	list_free(cached_privs_roles);
	cached_privs_roles = new_cached_privs_roles;
	cached_privs_role = roleid;

	/* And now we can return the answer */
	return cached_privs_roles;
}


/*
 * Get a list of roles that the specified roleid is a member of
 *
 * This is defined to recurse through roles regardless of rolinherit.
 *
 * Since indirect membership testing is relatively expensive, we cache
 * a list of memberships.  Hence, the result is only guaranteed good until
 * the next call of roles_is_member_of()!
 */
static List *
roles_is_member_of(Oid roleid)
{
	List	   *roles_list;
	ListCell   *l;
	List	   *new_cached_membership_roles;
	MemoryContext oldctx;

	/* If cache is already valid, just return the list */
	if (OidIsValid(cached_member_role) && cached_member_role == roleid)
		return cached_membership_roles;

	/*
	 * Find all the roles that roleid is a member of, including multi-level
	 * recursion.  The role itself will always be the first element of the
	 * resulting list.
	 *
	 * Each element of the list is scanned to see if it adds any indirect
	 * memberships.  We can use a single list as both the record of
	 * already-found memberships and the agenda of roles yet to be scanned.
	 * This is a bit tricky but works because the foreach() macro doesn't
	 * fetch the next list element until the bottom of the loop.
	 */
	roles_list = list_make1_oid(roleid);

	foreach(l, roles_list)
	{
		Oid			memberid = lfirst_oid(l);
		CatCList   *memlist;
		int			i;

		/* Find roles that memberid is directly a member of */
		memlist = SearchSysCacheList(AUTHMEMMEMROLE, 1,
									 ObjectIdGetDatum(memberid),
									 0, 0, 0);
		for (i = 0; i < memlist->n_members; i++)
		{
			HeapTuple	tup = &memlist->members[i]->tuple;
			Oid			otherid = ((Form_pg_auth_members) GETSTRUCT(tup))->roleid;

			/*
			 * Even though there shouldn't be any loops in the membership
			 * graph, we must test for having already seen this role. It is
			 * legal for instance to have both A->B and A->C->B.
			 */
			roles_list = list_append_unique_oid(roles_list, otherid);
		}
		ReleaseSysCacheList(memlist);
	}

	/*
	 * Copy the completed list into TopMemoryContext so it will persist.
	 */
	oldctx = MemoryContextSwitchTo(TopMemoryContext);
	new_cached_membership_roles = list_copy(roles_list);
	MemoryContextSwitchTo(oldctx);
	list_free(roles_list);

	/*
	 * Now safe to assign to state variable
	 */
	cached_member_role = InvalidOid;	/* just paranoia */
	list_free(cached_membership_roles);
	cached_membership_roles = new_cached_membership_roles;
	cached_member_role = roleid;

	/* And now we can return the answer */
	return cached_membership_roles;
}


/*
 * Does member have the privileges of role (directly or indirectly)?
 *
 * This is defined not to recurse through roles that don't have rolinherit
 * set; for such roles, membership implies the ability to do SET ROLE, but
 * the privileges are not available until you've done so.
 */
bool
has_privs_of_role(Oid member, Oid role)
{
	/* Fast path for simple case */
	if (member == role)
		return true;

	/* Superusers have every privilege, so are part of every role */
	if (superuser_arg(member))
		return true;

	/*
	 * Find all the roles that member has the privileges of, including
	 * multi-level recursion, then see if target role is any one of them.
	 */
	return list_member_oid(roles_has_privs_of(member), role);
}


/*
 * Is member a member of role (directly or indirectly)?
 *
 * This is defined to recurse through roles regardless of rolinherit.
 */
bool
is_member_of_role(Oid member, Oid role)
{
	/* Fast path for simple case */
	if (member == role)
		return true;

	/* Superusers have every privilege, so are part of every role */
	if (superuser_arg(member))
		return true;

	/*
	 * Find all the roles that member is a member of, including multi-level
	 * recursion, then see if target role is any one of them.
	 */
	return list_member_oid(roles_is_member_of(member), role);
}

/*
 * check_is_member_of_role
 *		is_member_of_role with a standard permission-violation error if not
 */
void
check_is_member_of_role(Oid member, Oid role)
{
	if (!is_member_of_role(member, role))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be member of role \"%s\"",
						GetUserNameFromId(role))));
}

/*
 * Is member a member of role, not considering superuserness?
 *
 * This is identical to is_member_of_role except we ignore superuser
 * status.
 */
bool
is_member_of_role_nosuper(Oid member, Oid role)
{
	/* Fast path for simple case */
	if (member == role)
		return true;

	/*
	 * Find all the roles that member is a member of, including multi-level
	 * recursion, then see if target role is any one of them.
	 */
	return list_member_oid(roles_is_member_of(member), role);
}


/*
 * Is member an admin of role (directly or indirectly)?  That is, is it
 * a member WITH ADMIN OPTION?
 *
 * We could cache the result as for is_member_of_role, but currently this
 * is not used in any performance-critical paths, so we don't.
 */
bool
is_admin_of_role(Oid member, Oid role)
{
	bool		result = false;
	List	   *roles_list;
	ListCell   *l;

	/* Fast path for simple case */
	if (member == role)
		return true;

	/* Superusers have every privilege, so are part of every role */
	if (superuser_arg(member))
		return true;

	/*
	 * Find all the roles that member is a member of, including multi-level
	 * recursion.  We build a list in the same way that is_member_of_role does
	 * to track visited and unvisited roles.
	 */
	roles_list = list_make1_oid(member);

	foreach(l, roles_list)
	{
		Oid			memberid = lfirst_oid(l);
		CatCList   *memlist;
		int			i;

		/* Find roles that memberid is directly a member of */
		memlist = SearchSysCacheList(AUTHMEMMEMROLE, 1,
									 ObjectIdGetDatum(memberid),
									 0, 0, 0);
		for (i = 0; i < memlist->n_members; i++)
		{
			HeapTuple	tup = &memlist->members[i]->tuple;
			Oid			otherid = ((Form_pg_auth_members) GETSTRUCT(tup))->roleid;

			if (otherid == role &&
				((Form_pg_auth_members) GETSTRUCT(tup))->admin_option)
			{
				/* Found what we came for, so can stop searching */
				result = true;
				break;
			}

			roles_list = list_append_unique_oid(roles_list, otherid);
		}
		ReleaseSysCacheList(memlist);
		if (result)
			break;
	}

	list_free(roles_list);

	return result;
}


/* does what it says ... */
static int
count_one_bits(AclMode mask)
{
	int			nbits = 0;

	/* this code relies on AclMode being an unsigned type */
	while (mask)
	{
		if (mask & 1)
			nbits++;
		mask >>= 1;
	}
	return nbits;
}


/*
 * Select the effective grantor ID for a GRANT or REVOKE operation.
 *
 * The grantor must always be either the object owner or some role that has
 * been explicitly granted grant options.  This ensures that all granted
 * privileges appear to flow from the object owner, and there are never
 * multiple "original sources" of a privilege.	Therefore, if the would-be
 * grantor is a member of a role that has the needed grant options, we have
 * to do the grant as that role instead.
 *
 * It is possible that the would-be grantor is a member of several roles
 * that have different subsets of the desired grant options, but no one
 * role has 'em all.  In this case we pick a role with the largest number
 * of desired options.	Ties are broken in favor of closer ancestors.
 *
 * roleId: the role attempting to do the GRANT/REVOKE
 * privileges: the privileges to be granted/revoked
 * acl: the ACL of the object in question
 * ownerId: the role owning the object in question
 * *grantorId: receives the OID of the role to do the grant as
 * *grantOptions: receives the grant options actually held by grantorId
 *
 * If no grant options exist, we set grantorId to roleId, grantOptions to 0.
 */
void
select_best_grantor(Oid roleId, AclMode privileges,
					const Acl *acl, Oid ownerId,
					Oid *grantorId, AclMode *grantOptions)
{
	AclMode		needed_goptions = ACL_GRANT_OPTION_FOR(privileges);
	List	   *roles_list;
	int			nrights;
	ListCell   *l;

	/*
	 * The object owner is always treated as having all grant options, so if
	 * roleId is the owner it's easy.  Also, if roleId is a superuser it's
	 * easy: superusers are implicitly members of every role, so they act as
	 * the object owner.
	 */
	if (roleId == ownerId || superuser_arg(roleId))
	{
		*grantorId = ownerId;
		*grantOptions = needed_goptions;
		return;
	}

	/*
	 * Otherwise we have to do a careful search to see if roleId has the
	 * privileges of any suitable role.  Note: we can hang onto the result of
	 * roles_has_privs_of() throughout this loop, because aclmask_direct()
	 * doesn't query any role memberships.
	 */
	roles_list = roles_has_privs_of(roleId);

	/* initialize candidate result as default */
	*grantorId = roleId;
	*grantOptions = ACL_NO_RIGHTS;
	nrights = 0;

	foreach(l, roles_list)
	{
		Oid			otherrole = lfirst_oid(l);
		AclMode		otherprivs;

		otherprivs = aclmask_direct(acl, otherrole, ownerId,
									needed_goptions, ACLMASK_ALL);
		if (otherprivs == needed_goptions)
		{
			/* Found a suitable grantor */
			*grantorId = otherrole;
			*grantOptions = otherprivs;
			return;
		}

		/*
		 * If it has just some of the needed privileges, remember best
		 * candidate.
		 */
		if (otherprivs != ACL_NO_RIGHTS)
		{
			int			nnewrights = count_one_bits(otherprivs);

			if (nnewrights > nrights)
			{
				*grantorId = otherrole;
				*grantOptions = otherprivs;
				nrights = nnewrights;
			}
		}
	}
}
