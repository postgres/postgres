/*-------------------------------------------------------------------------
 *
 * acl.c
 *	  Basic access control list data structures manipulation routines.
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/adt/acl.c,v 1.72 2002/04/26 01:24:08 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>

#include "catalog/namespace.h"
#include "catalog/pg_shadow.h"
#include "miscadmin.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"


#define ACL_IDTYPE_GID_KEYWORD	"group"
#define ACL_IDTYPE_UID_KEYWORD	"user"

static const char *getid(const char *s, char *n);
static Acl *makeacl(int n);
static const char *aclparse(const char *s, AclItem *aip, unsigned *modechg);
static bool aclitemeq(const AclItem *a1, const AclItem *a2);
static bool aclitemgt(const AclItem *a1, const AclItem *a2);

static AclMode convert_priv_string(text *priv_type_text);
static Oid convert_rel_name(text *relname);


/*
 * getid
 *		Consumes the first alphanumeric string (identifier) found in string
 *		's', ignoring any leading white space.	If it finds a double quote
 *		it returns the word inside the quotes.
 *
 * RETURNS:
 *		the string position in 's' that points to the next non-space character
 *		in 's', after any quotes.  Also:
 *		- loads the identifier into 'name'.  (If no identifier is found, 'name'
 *		  contains an empty string.)  name must be NAMEDATALEN bytes.
 */
static const char *
getid(const char *s, char *n)
{
	unsigned	len;
	const char *id;
	int			in_quotes = 0;

	Assert(s && n);

	while (isspace((unsigned char) *s))
		++s;

	if (*s == '"')
	{
		in_quotes = 1;
		s++;
	}

	for (id = s, len = 0;
		 isalnum((unsigned char) *s) || *s == '_' || in_quotes;
		 ++len, ++s)
	{
		if (in_quotes && *s == '"')
		{
			len--;
			in_quotes = 0;
		}
	}
	if (len >= NAMEDATALEN)
		elog(ERROR, "getid: identifier must be <%d characters",
			 NAMEDATALEN);
	if (len > 0)
		memmove(n, id, len);
	n[len] = '\0';
	while (isspace((unsigned char) *s))
		++s;
	return s;
}

/*
 * aclparse
 *		Consumes and parses an ACL specification of the form:
 *				[group|user] [A-Za-z0-9]*[+-=][rwaR]*
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
 *		- loads 'modechg' with the mode change flag.
 */
static const char *
aclparse(const char *s, AclItem *aip, unsigned *modechg)
{
	AclMode		privs;
	uint32		idtype;
	char		name[NAMEDATALEN];

	Assert(s && aip && modechg);

#ifdef ACLDEBUG
	elog(LOG, "aclparse: input = '%s'", s);
#endif
	idtype = ACL_IDTYPE_UID;
	s = getid(s, name);
	if (*s != ACL_MODECHG_ADD_CHR &&
		*s != ACL_MODECHG_DEL_CHR &&
		*s != ACL_MODECHG_EQL_CHR)
	{
		/* we just read a keyword, not a name */
		if (strcmp(name, ACL_IDTYPE_GID_KEYWORD) == 0)
			idtype = ACL_IDTYPE_GID;
		else if (strcmp(name, ACL_IDTYPE_UID_KEYWORD) != 0)
			elog(ERROR, "aclparse: bad keyword, must be [group|user]");
		s = getid(s, name);		/* move s to the name beyond the keyword */
		if (name[0] == '\0')
			elog(ERROR, "aclparse: a name must follow the [group|user] keyword");
	}
	if (name[0] == '\0')
		idtype = ACL_IDTYPE_WORLD;

	switch (*s)
	{
		case ACL_MODECHG_ADD_CHR:
			*modechg = ACL_MODECHG_ADD;
			break;
		case ACL_MODECHG_DEL_CHR:
			*modechg = ACL_MODECHG_DEL;
			break;
		case ACL_MODECHG_EQL_CHR:
			*modechg = ACL_MODECHG_EQL;
			break;
		default:
			elog(ERROR, "aclparse: mode change flag must use \"%c%c%c\"",
				 ACL_MODECHG_ADD_CHR,
				 ACL_MODECHG_DEL_CHR,
				 ACL_MODECHG_EQL_CHR);
	}

	privs = ACL_NO_RIGHTS;

	while (isalpha((unsigned char) *++s))
	{
		switch (*s)
		{
			case ACL_INSERT_CHR:
				privs |= ACL_INSERT;
				break;
			case ACL_SELECT_CHR:
				privs |= ACL_SELECT;
				break;
			case ACL_UPDATE_CHR:
				privs |= ACL_UPDATE;
				break;
			case ACL_DELETE_CHR:
				privs |= ACL_DELETE;
				break;
			case ACL_RULE_CHR:
				privs |= ACL_RULE;
				break;
			case ACL_REFERENCES_CHR:
				privs |= ACL_REFERENCES;
				break;
			case ACL_TRIGGER_CHR:
				privs |= ACL_TRIGGER;
				break;
			case ACL_EXECUTE_CHR:
				privs |= ACL_EXECUTE;
				break;
			case ACL_USAGE_CHR:
				privs |= ACL_USAGE;
				break;
			case ACL_CREATE_CHR:
				privs |= ACL_CREATE;
				break;
			case ACL_CREATE_TEMP_CHR:
				privs |= ACL_CREATE_TEMP;
				break;
			default:
				elog(ERROR, "aclparse: mode flags must use \"%s\"",
					 ACL_ALL_RIGHTS_STR);
		}
	}

	switch (idtype)
	{
		case ACL_IDTYPE_UID:
			aip->ai_id = get_usesysid(name);
			break;
		case ACL_IDTYPE_GID:
			aip->ai_id = get_grosysid(name);
			break;
		case ACL_IDTYPE_WORLD:
			aip->ai_id = ACL_ID_WORLD;
			break;
	}

	ACLITEM_SET_PRIVS_IDTYPE(*aip, privs, idtype);

#ifdef ACLDEBUG
	elog(LOG, "aclparse: correctly read [%x %d %x], modechg=%x",
		 idtype, aip->ai_id, privs, *modechg);
#endif
	return s;
}

/*
 * makeacl
 *		Allocates storage for a new Acl with 'n' entries.
 *
 * RETURNS:
 *		the new Acl
 */
static Acl *
makeacl(int n)
{
	Acl		   *new_acl;
	Size		size;

	if (n < 0)
		elog(ERROR, "makeacl: invalid size: %d", n);
	size = ACL_N_SIZE(n);
	new_acl = (Acl *) palloc(size);
	MemSet((char *) new_acl, 0, size);
	new_acl->size = size;
	new_acl->ndim = 1;
	new_acl->flags = 0;
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
	unsigned	modechg;

	aip = (AclItem *) palloc(sizeof(AclItem));
	s = aclparse(s, aip, &modechg);
	if (modechg != ACL_MODECHG_EQL)
		elog(ERROR, "aclitemin: cannot accept anything but = ACLs");
	while (isspace((unsigned char) *s))
		++s;
	if (*s)
		elog(ERROR, "aclitemin: extra garbage at end of specification");
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

	p = out = palloc(strlen("group = ") + N_ACL_RIGHTS + NAMEDATALEN + 1);
	*p = '\0';

	switch (ACLITEM_GET_IDTYPE(*aip))
	{
		case ACL_IDTYPE_UID:
			htup = SearchSysCache(SHADOWSYSID,
								  ObjectIdGetDatum(aip->ai_id),
								  0, 0, 0);
			if (HeapTupleIsValid(htup))
			{
				strncat(p,
					NameStr(((Form_pg_shadow) GETSTRUCT(htup))->usename),
						NAMEDATALEN);
				ReleaseSysCache(htup);
			}
			else
			{
				/* Generate numeric UID if we don't find an entry */
				char	   *tmp;

				tmp = DatumGetCString(DirectFunctionCall1(int4out,
									 Int32GetDatum((int32) aip->ai_id)));
				strcat(p, tmp);
				pfree(tmp);
			}
			break;
		case ACL_IDTYPE_GID:
			strcat(p, "group ");
			tmpname = get_groname(aip->ai_id);
			if (tmpname != NULL)
				strncat(p, tmpname, NAMEDATALEN);
			else
			{
				/* Generate numeric GID if we don't find an entry */
				char	   *tmp;

				tmp = DatumGetCString(DirectFunctionCall1(int4out,
									 Int32GetDatum((int32) aip->ai_id)));
				strcat(p, tmp);
				pfree(tmp);
			}
			break;
		case ACL_IDTYPE_WORLD:
			break;
		default:
			elog(ERROR, "aclitemout: bad idtype: %d",
				 ACLITEM_GET_IDTYPE(*aip));
			break;
	}
	while (*p)
		++p;
	*p++ = '=';
	for (i = 0; i < N_ACL_RIGHTS; ++i)
		if (aip->ai_privs & (1 << i))
			*p++ = ACL_ALL_RIGHTS_STR[i];
	*p = '\0';

	PG_RETURN_CSTRING(out);
}

/*
 * aclitemeq
 * aclitemgt
 *		AclItem equality and greater-than comparison routines.
 *		Two AclItems are considered equal iff they have the same
 *		identifier (and identifier type); the privileges are ignored.
 *		Note that these routines are really only useful for sorting
 *		AclItems into identifier order.
 *
 * RETURNS:
 *		a boolean value indicating = or >
 */
static bool
aclitemeq(const AclItem *a1, const AclItem *a2)
{
	return ACLITEM_GET_IDTYPE(*a1) == ACLITEM_GET_IDTYPE(*a2) &&
		a1->ai_id == a2->ai_id;
}

static bool
aclitemgt(const AclItem *a1, const AclItem *a2)
{
	return ((ACLITEM_GET_IDTYPE(*a1) > ACLITEM_GET_IDTYPE(*a2)) ||
			(ACLITEM_GET_IDTYPE(*a1) == ACLITEM_GET_IDTYPE(*a2) &&
			 a1->ai_id > a2->ai_id));
}


/*
 * acldefault()  --- create an ACL describing default access permissions
 *
 * Change this routine if you want to alter the default access policy for
 * newly-created tables (or any table with a NULL acl entry in pg_class)
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
			world_default = ACL_NO_RIGHTS;
			owner_default = ACL_ALL_RIGHTS_DATABASE;
			break;
		case ACL_OBJECT_FUNCTION:
			world_default = ACL_NO_RIGHTS;
			owner_default = ACL_ALL_RIGHTS_FUNCTION;
			break;
		case ACL_OBJECT_LANGUAGE:
			world_default = ACL_NO_RIGHTS;
			owner_default = ACL_ALL_RIGHTS_LANGUAGE;
			break;
		case ACL_OBJECT_NAMESPACE:
			world_default = ACL_NO_RIGHTS;
			owner_default = ACL_ALL_RIGHTS_NAMESPACE;
			break;
		default:
			elog(ERROR, "acldefault: bogus objtype %d", (int) objtype);
			world_default = ACL_NO_RIGHTS; /* keep compiler quiet */
			owner_default = ACL_NO_RIGHTS;
			break;
	}

	acl = makeacl(ownerid ? 2 : 1);
	aip = ACL_DAT(acl);

	aip[0].ai_id = ACL_ID_WORLD;
	ACLITEM_SET_PRIVS_IDTYPE(aip[0], world_default, ACL_IDTYPE_WORLD);
	if (ownerid)
	{
		aip[1].ai_id = ownerid;
		ACLITEM_SET_PRIVS_IDTYPE(aip[1], owner_default, ACL_IDTYPE_UID);
	}

	return acl;
}


/*
 * Add or replace an item in an ACL array.	The result is a modified copy;
 * the input object is not changed.
 *
 * NB: caller is responsible for having detoasted the input ACL, if needed.
 */
Acl *
aclinsert3(const Acl *old_acl, const AclItem *mod_aip, unsigned modechg)
{
	Acl		   *new_acl;
	AclItem    *old_aip,
			   *new_aip;
	int			dst,
				num;

	/* These checks for null input are probably dead code, but... */
	if (!old_acl || ACL_NUM(old_acl) < 1)
		old_acl = makeacl(1);
	if (!mod_aip)
	{
		new_acl = makeacl(ACL_NUM(old_acl));
		memcpy((char *) new_acl, (char *) old_acl, ACL_SIZE(old_acl));
		return new_acl;
	}

	num = ACL_NUM(old_acl);
	old_aip = ACL_DAT(old_acl);

	/*
	 * Search the ACL for an existing entry for 'id'.  If one exists, just
	 * modify the entry in-place (well, in the same position, since we
	 * actually return a copy); otherwise, insert the new entry in
	 * sort-order.
	 */
	/* find the first element not less than the element to be inserted */
	for (dst = 0; dst < num && aclitemgt(mod_aip, old_aip + dst); ++dst)
		;

	if (dst < num && aclitemeq(mod_aip, old_aip + dst))
	{
		/* found a match, so modify existing item */
		new_acl = makeacl(num);
		new_aip = ACL_DAT(new_acl);
		memcpy((char *) new_acl, (char *) old_acl, ACL_SIZE(old_acl));
	}
	else
	{
		/* need to insert a new item */
		new_acl = makeacl(num + 1);
		new_aip = ACL_DAT(new_acl);
		if (dst == 0)
		{						/* start */
			elog(ERROR, "aclinsert3: insertion before world ACL??");
		}
		else if (dst >= num)
		{						/* end */
			memcpy((char *) new_aip,
				   (char *) old_aip,
				   num * sizeof(AclItem));
		}
		else
		{						/* middle */
			memcpy((char *) new_aip,
				   (char *) old_aip,
				   dst * sizeof(AclItem));
			memcpy((char *) (new_aip + dst + 1),
				   (char *) (old_aip + dst),
				   (num - dst) * sizeof(AclItem));
		}
		/* initialize the new entry with no permissions */
		new_aip[dst].ai_id = mod_aip->ai_id;
		ACLITEM_SET_PRIVS_IDTYPE(new_aip[dst], ACL_NO_RIGHTS,
								 ACLITEM_GET_IDTYPE(*mod_aip));
		num++;					/* set num to the size of new_acl */
	}

	/* apply the permissions mod */
	switch (modechg)
	{
		case ACL_MODECHG_ADD:
			new_aip[dst].ai_privs |= ACLITEM_GET_PRIVS(*mod_aip);
			break;
		case ACL_MODECHG_DEL:
			new_aip[dst].ai_privs &= ~ACLITEM_GET_PRIVS(*mod_aip);
			break;
		case ACL_MODECHG_EQL:
			ACLITEM_SET_PRIVS_IDTYPE(new_aip[dst],
									 ACLITEM_GET_PRIVS(*mod_aip),
									 ACLITEM_GET_IDTYPE(new_aip[dst]));
			break;
	}

	/*
	 * if the adjusted entry has no permissions, delete it from the list.
	 * For example, this helps in removing entries for users who no longer
	 * exist.  EXCEPTION: never remove the world entry.
	 */
	if (ACLITEM_GET_PRIVS(new_aip[dst]) == ACL_NO_RIGHTS && dst > 0)
	{
		memmove((char *) (new_aip + dst),
				(char *) (new_aip + dst + 1),
				(num - dst - 1) * sizeof(AclItem));
		ARR_DIMS(new_acl)[0] = num - 1;
		ARR_SIZE(new_acl) -= sizeof(AclItem);
	}

	return new_acl;
}

/*
 * aclinsert (exported function)
 */
Datum
aclinsert(PG_FUNCTION_ARGS)
{
	Acl		   *old_acl = PG_GETARG_ACL_P(0);
	AclItem    *mod_aip = PG_GETARG_ACLITEM_P(1);

	PG_RETURN_ACL_P(aclinsert3(old_acl, mod_aip, ACL_MODECHG_EQL));
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
	if (!old_acl || ACL_NUM(old_acl) < 1)
		old_acl = makeacl(1);
	if (!mod_aip)
	{
		new_acl = makeacl(ACL_NUM(old_acl));
		memcpy((char *) new_acl, (char *) old_acl, ACL_SIZE(old_acl));
		PG_RETURN_ACL_P(new_acl);
	}

	old_num = ACL_NUM(old_acl);
	old_aip = ACL_DAT(old_acl);

	/* Search for the matching entry */
	for (dst = 0; dst < old_num && !aclitemeq(mod_aip, old_aip + dst); ++dst)
		;

	if (dst >= old_num)
	{
		/* Not found, so return copy of source ACL */
		new_acl = makeacl(old_num);
		memcpy((char *) new_acl, (char *) old_acl, ACL_SIZE(old_acl));
	}
	else
	{
		new_num = old_num - 1;
		new_acl = makeacl(new_num);
		new_aip = ACL_DAT(new_acl);
		if (dst == 0)
		{						/* start */
			elog(ERROR, "aclremove: removal of the world ACL??");
		}
		else if (dst == old_num - 1)
		{						/* end */
			memcpy((char *) new_aip,
				   (char *) old_aip,
				   new_num * sizeof(AclItem));
		}
		else
		{						/* middle */
			memcpy((char *) new_aip,
				   (char *) old_aip,
				   dst * sizeof(AclItem));
			memcpy((char *) (new_aip + dst),
				   (char *) (old_aip + dst + 1),
				   (new_num - dst) * sizeof(AclItem));
		}
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
		if (aip->ai_id == aidat[i].ai_id &&
			aip->ai_privs == aidat[i].ai_privs)
			PG_RETURN_BOOL(true);
	}
	PG_RETURN_BOOL(false);
}


/*
 * has_table_privilege_name_name
 *		Check user privileges on a relation given
 *		name username, text relname, and text priv name.
 *
 * RETURNS
 *		a boolean value
 *		't' indicating user has the privilege
 *		'f' indicating user does not have the privilege
 */
Datum
has_table_privilege_name_name(PG_FUNCTION_ARGS)
{
	Name		username = PG_GETARG_NAME(0);
	text	   *relname = PG_GETARG_TEXT_P(1);
	text	   *priv_type_text = PG_GETARG_TEXT_P(2);
	int32		usesysid;
	Oid			reloid;
	AclMode		mode;
	int32		aclresult;

	/*
	 * Lookup userid based on username
	 */
	usesysid = get_usesysid(NameStr(*username));

	/*
	 * Lookup rel OID based on relname
	 */
	reloid = convert_rel_name(relname);

	/*
	 * Convert priv_type_text to an AclMode
	 */
	mode = convert_priv_string(priv_type_text);

	/*
	 * Check for the privilege
	 */
	aclresult = pg_class_aclcheck(reloid, usesysid, mode);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}


/*
 * has_table_privilege_name
 *		Check user privileges on a relation given
 *		text relname and text priv name.
 *		current_user is assumed
 *
 * RETURNS
 *		a boolean value
 *		't' indicating user has the privilege
 *		'f' indicating user does not have the privilege
 */
Datum
has_table_privilege_name(PG_FUNCTION_ARGS)
{
	text	   *relname = PG_GETARG_TEXT_P(0);
	text	   *priv_type_text = PG_GETARG_TEXT_P(1);
	int32		usesysid;
	Oid			reloid;
	AclMode		mode;
	int32		aclresult;

	usesysid = GetUserId();

	/*
	 * Lookup rel OID based on relname
	 */
	reloid = convert_rel_name(relname);

	/*
	 * Convert priv_type_text to an AclMode
	 */
	mode = convert_priv_string(priv_type_text);

	/*
	 * Check for the privilege
	 */
	aclresult = pg_class_aclcheck(reloid, usesysid, mode);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}


/*
 * has_table_privilege_name_id
 *		Check user privileges on a relation given
 *		name usename, rel oid, and text priv name.
 *
 * RETURNS
 *		a boolean value
 *		't' indicating user has the privilege
 *		'f' indicating user does not have the privilege
 */
Datum
has_table_privilege_name_id(PG_FUNCTION_ARGS)
{
	Name		username = PG_GETARG_NAME(0);
	Oid			reloid = PG_GETARG_OID(1);
	text	   *priv_type_text = PG_GETARG_TEXT_P(2);
	int32		usesysid;
	AclMode		mode;
	int32		aclresult;

	/*
	 * Lookup userid based on username
	 */
	usesysid = get_usesysid(NameStr(*username));

	/*
	 * Convert priv_type_text to an AclMode
	 */
	mode = convert_priv_string(priv_type_text);

	/*
	 * Check for the privilege
	 */
	aclresult = pg_class_aclcheck(reloid, usesysid, mode);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}


/*
 * has_table_privilege_id
 *		Check user privileges on a relation given
 *		rel oid, and text priv name.
 *		current_user is assumed
 *
 * RETURNS
 *		a boolean value
 *		't' indicating user has the privilege
 *		'f' indicating user does not have the privilege
 */
Datum
has_table_privilege_id(PG_FUNCTION_ARGS)
{
	Oid			reloid = PG_GETARG_OID(0);
	text	   *priv_type_text = PG_GETARG_TEXT_P(1);
	int32		usesysid;
	AclMode		mode;
	int32		aclresult;

	usesysid = GetUserId();

	/*
	 * Convert priv_type_text to an AclMode
	 */
	mode = convert_priv_string(priv_type_text);

	/*
	 * Check for the privilege
	 */
	aclresult = pg_class_aclcheck(reloid, usesysid, mode);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}


/*
 * has_table_privilege_id_name
 *		Check user privileges on a relation given
 *		usesysid, text relname, and priv name.
 *
 * RETURNS
 *		a boolean value
 *		't' indicating user has the privilege
 *		'f' indicating user does not have the privilege
 */
Datum
has_table_privilege_id_name(PG_FUNCTION_ARGS)
{
	int32		usesysid = PG_GETARG_INT32(0);
	text	   *relname = PG_GETARG_TEXT_P(1);
	text	   *priv_type_text = PG_GETARG_TEXT_P(2);
	Oid			reloid;
	AclMode		mode;
	int32		aclresult;

	/*
	 * Lookup rel OID based on relname
	 */
	reloid = convert_rel_name(relname);

	/*
	 * Convert priv_type_text to an AclMode
	 */
	mode = convert_priv_string(priv_type_text);

	/*
	 * Check for the privilege
	 */
	aclresult = pg_class_aclcheck(reloid, usesysid, mode);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}


/*
 * has_table_privilege_id_id
 *		Check user privileges on a relation given
 *		usesysid, rel oid, and priv name.
 *
 * RETURNS
 *		a boolean value
 *		't' indicating user has the privilege
 *		'f' indicating user does not have the privilege
 */
Datum
has_table_privilege_id_id(PG_FUNCTION_ARGS)
{
	int32		usesysid = PG_GETARG_INT32(0);
	Oid			reloid = PG_GETARG_OID(1);
	text	   *priv_type_text = PG_GETARG_TEXT_P(2);
	AclMode		mode;
	int32		aclresult;

	/*
	 * Convert priv_type_text to an AclMode
	 */
	mode = convert_priv_string(priv_type_text);

	/*
	 * Check for the privilege
	 */
	aclresult = pg_class_aclcheck(reloid, usesysid, mode);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}

/*
 *		Internal functions.
 */

/*
 * Given a relation name expressed as a string, look it up and return Oid
 */
static Oid
convert_rel_name(text *relname)
{
	RangeVar   *relrv;

	relrv = makeRangeVarFromNameList(textToQualifiedNameList(relname,
													"has_table_privilege"));

	return RangeVarGetRelid(relrv, false);
}

/*
 * convert_priv_string
 *		Internal function.
 *		Return mode from priv_type string
 *
 * RETURNS
 *		AclMode
 */
static AclMode
convert_priv_string(text *priv_type_text)
{
	char	   *priv_type;

	priv_type = DatumGetCString(DirectFunctionCall1(textout,
											PointerGetDatum(priv_type_text)));

	/*
	 * Return mode from priv_type string
	 */
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

	elog(ERROR, "has_table_privilege: invalid privilege type %s", priv_type);

	/*
	 * We should never get here, but stop the compiler from complaining
	 */
	return ACL_NO_RIGHTS;
}
