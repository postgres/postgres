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
 *	  $Header: /cvsroot/pgsql/src/backend/utils/adt/acl.c,v 1.69 2002/03/21 23:27:24 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>

#include "access/heapam.h"
#include "catalog/catalog.h"
#include "catalog/pg_shadow.h"
#include "catalog/pg_type.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"


#define ACL_IDTYPE_GID_KEYWORD	"group"
#define ACL_IDTYPE_UID_KEYWORD	"user"

static const char *getid(const char *s, char *n);
static bool aclitemeq(const AclItem *a1, const AclItem *a2);
static bool aclitemgt(const AclItem *a1, const AclItem *a2);

static AclMode convert_priv_string(text *priv_type_text);
static bool has_table_privilege_cname_cname(char *username, char *relname,
								text *priv_type_text);
static bool has_table_privilege_cname_id(char *username, Oid reloid,
							 text *priv_type_text);
static bool has_table_privilege_id_cname(int32 usesysid, char *relname,
							 text *priv_type_text);


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
const char *
aclparse(const char *s, AclItem *aip, unsigned *modechg)
{
	char		name[NAMEDATALEN];

	Assert(s && aip && modechg);

#ifdef ACLDEBUG
	elog(LOG, "aclparse: input = '%s'", s);
#endif
	aip->ai_idtype = ACL_IDTYPE_UID;
	s = getid(s, name);
	if (*s != ACL_MODECHG_ADD_CHR &&
		*s != ACL_MODECHG_DEL_CHR &&
		*s != ACL_MODECHG_EQL_CHR)
	{
		/* we just read a keyword, not a name */
		if (!strcmp(name, ACL_IDTYPE_GID_KEYWORD))
			aip->ai_idtype = ACL_IDTYPE_GID;
		else if (strcmp(name, ACL_IDTYPE_UID_KEYWORD))
			elog(ERROR, "aclparse: bad keyword, must be [group|user]");
		s = getid(s, name);		/* move s to the name beyond the keyword */
		if (name[0] == '\0')
			elog(ERROR, "aclparse: a name must follow the [group|user] keyword");
	}
	if (name[0] == '\0')
		aip->ai_idtype = ACL_IDTYPE_WORLD;

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
			elog(ERROR, "aclparse: mode change flag must use \"%s\"",
				 ACL_MODECHG_STR);
	}

	aip->ai_mode = ACL_NO;
	while (isalpha((unsigned char) *++s))
	{
		switch (*s)
		{
			case ACL_MODE_INSERT_CHR:
				aip->ai_mode |= ACL_INSERT;
				break;
			case ACL_MODE_SELECT_CHR:
				aip->ai_mode |= ACL_SELECT;
				break;
			case ACL_MODE_UPDATE_CHR:
				aip->ai_mode |= ACL_UPDATE;
				break;
			case ACL_MODE_DELETE_CHR:
				aip->ai_mode |= ACL_DELETE;
				break;
			case ACL_MODE_RULE_CHR:
				aip->ai_mode |= ACL_RULE;
				break;
			case ACL_MODE_REFERENCES_CHR:
				aip->ai_mode |= ACL_REFERENCES;
				break;
			case ACL_MODE_TRIGGER_CHR:
				aip->ai_mode |= ACL_TRIGGER;
				break;
			default:
				elog(ERROR, "aclparse: mode flags must use \"%s\"",
					 ACL_MODE_STR);
		}
	}

	switch (aip->ai_idtype)
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

#ifdef ACLDEBUG
	elog(LOG, "aclparse: correctly read [%x %d %x], modechg=%x",
		 aip->ai_idtype, aip->ai_id, aip->ai_mode, *modechg);
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
Acl *
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

	p = out = palloc(strlen("group =" ACL_MODE_STR " ") + 1 + NAMEDATALEN);
	*p = '\0';

	switch (aip->ai_idtype)
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
			elog(ERROR, "aclitemout: bad ai_idtype: %d", aip->ai_idtype);
			break;
	}
	while (*p)
		++p;
	*p++ = '=';
	for (i = 0; i < N_ACL_MODES; ++i)
		if ((aip->ai_mode >> i) & 01)
			*p++ = ACL_MODE_STR[i];
	*p = '\0';

	PG_RETURN_CSTRING(out);
}

/*
 * aclitemeq
 * aclitemgt
 *		AclItem equality and greater-than comparison routines.
 *		Two AclItems are considered equal iff they have the
 *		same identifier (and identifier type); the mode is ignored.
 *		Note that these routines are really only useful for sorting
 *		AclItems into identifier order.
 *
 * RETURNS:
 *		a boolean value indicating = or >
 */
static bool
aclitemeq(const AclItem *a1, const AclItem *a2)
{
	return a1->ai_idtype == a2->ai_idtype && a1->ai_id == a2->ai_id;
}

static bool
aclitemgt(const AclItem *a1, const AclItem *a2)
{
	return ((a1->ai_idtype > a2->ai_idtype) ||
			(a1->ai_idtype == a2->ai_idtype && a1->ai_id > a2->ai_id));
}


/*
 * acldefault()  --- create an ACL describing default access permissions
 *
 * Change this routine if you want to alter the default access policy for
 * newly-created tables (or any table with a NULL acl entry in pg_class)
 */
Acl *
acldefault(AclId ownerid)
{
	Acl		   *acl;
	AclItem    *aip;

#define ACL_WORLD_DEFAULT		(ACL_NO)
#define ACL_OWNER_DEFAULT		(ACL_INSERT|ACL_SELECT|ACL_UPDATE|ACL_DELETE|ACL_RULE|ACL_REFERENCES|ACL_TRIGGER)

	acl = makeacl(ownerid ? 2 : 1);
	aip = ACL_DAT(acl);
	aip[0].ai_idtype = ACL_IDTYPE_WORLD;
	aip[0].ai_id = ACL_ID_WORLD;
	aip[0].ai_mode = ACL_WORLD_DEFAULT;
	/* FIXME: The owner's default should vary with the object type. */
	if (ownerid)
	{
		aip[1].ai_idtype = ACL_IDTYPE_UID;
		aip[1].ai_id = ownerid;
		aip[1].ai_mode = ACL_OWNER_DEFAULT;
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
		new_aip[dst].ai_idtype = mod_aip->ai_idtype;
		new_aip[dst].ai_mode = 0;
		num++;					/* set num to the size of new_acl */
	}

	/* apply the permissions mod */
	switch (modechg)
	{
		case ACL_MODECHG_ADD:
			new_aip[dst].ai_mode |= mod_aip->ai_mode;
			break;
		case ACL_MODECHG_DEL:
			new_aip[dst].ai_mode &= ~mod_aip->ai_mode;
			break;
		case ACL_MODECHG_EQL:
			new_aip[dst].ai_mode = mod_aip->ai_mode;
			break;
	}

	/*
	 * if the adjusted entry has no permissions, delete it from the list.
	 * For example, this helps in removing entries for users who no longer
	 * exist.  EXCEPTION: never remove the world entry.
	 */
	if (new_aip[dst].ai_mode == 0 && dst > 0)
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
		/* Note that aclitemeq only considers id, not mode */
		if (aclitemeq(aip, aidat + i) &&
			aip->ai_mode == aidat[i].ai_mode)
			PG_RETURN_BOOL(true);
	}
	PG_RETURN_BOOL(false);
}


/*
 * Parser support routines for ACL-related statements.
 *
 * XXX CAUTION: these are called from gram.y, which is not allowed to
 * do any table accesses.  Therefore, it is not kosher to do things
 * like trying to translate usernames to user IDs here.  Keep it all
 * in string form until statement execution time.
 */

/*
 * aclmakepriv
 *	  make a acl privilege string out of an existing privilege string
 * and a new privilege
 *
 * does not add duplicate privileges
 */
char *
aclmakepriv(const char *old_privlist, char new_priv)
{
	char	   *priv;
	int			i;
	int			l;

	Assert(strlen(old_privlist) <= strlen(ACL_MODE_STR));
	priv = palloc(strlen(ACL_MODE_STR) + 1);

	if (old_privlist == NULL || old_privlist[0] == '\0')
	{
		priv[0] = new_priv;
		priv[1] = '\0';
		return priv;
	}

	strcpy(priv, old_privlist);

	l = strlen(old_privlist);

	if (l == strlen(ACL_MODE_STR))
	{							/* can't add any more privileges */
		return priv;
	}

	/* check to see if the new privilege is already in the old string */
	for (i = 0; i < l; i++)
	{
		if (priv[i] == new_priv)
			break;
	}
	if (i == l)
	{							/* we really have a new privilege */
		priv[l] = new_priv;
		priv[l + 1] = '\0';
	}

	return priv;
}

/*
 * aclmakeuser
 *	  user_type must be "A"  - all users
 *						"G"  - group
 *						"U"  - user
 *
 * Just concatenates the two strings together with a space in between.
 * Per above comments, we can't try to resolve a user or group name here.
 */
char *
aclmakeuser(const char *user_type, const char *user)
{
	char	   *user_list;

	user_list = palloc(strlen(user_type) + strlen(user) + 2);
	sprintf(user_list, "%s %s", user_type, user);
	return user_list;
}


/*
 * makeAclString:  We take in the privileges and grantee as well as a
 * single character '+' or '-' to indicate grant or revoke.
 *
 * We convert the information to the same external form recognized by
 * aclitemin (see aclparse) and return that string.  Conversion to
 * internal form happens when the statement is executed.
 */
char *
makeAclString(const char *privileges, const char *grantee, char grant_or_revoke)
{
	StringInfoData str;
	char	   *ret;

	initStringInfo(&str);

	/* the grantee string is "G <group_name>", "U <user_name>", or "ALL" */
	if (grantee[0] == 'G')		/* group permissions */
	{
		appendStringInfo(&str, "%s \"%s\"%c%s",
						 ACL_IDTYPE_GID_KEYWORD,
						 grantee + 2, grant_or_revoke, privileges);
	}
	else if (grantee[0] == 'U') /* user permission */
	{
		appendStringInfo(&str, "%s \"%s\"%c%s",
						 ACL_IDTYPE_UID_KEYWORD,
						 grantee + 2, grant_or_revoke, privileges);
	}
	else
	{
		/* all permission */
		appendStringInfo(&str, "%c%s",
						 grant_or_revoke, privileges);
	}
	ret = pstrdup(str.data);
	pfree(str.data);
	return ret;
}


/*
 * has_table_privilege_name_name
 *		Check user privileges on a relation given
 *		name usename, name relname, and text priv name.
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
	Name		relname = PG_GETARG_NAME(1);
	text	   *priv_type_text = PG_GETARG_TEXT_P(2);
	bool		result;

	result = has_table_privilege_cname_cname(NameStr(*username),
											 NameStr(*relname),
											 priv_type_text);

	PG_RETURN_BOOL(result);
}


/*
 * has_table_privilege_name
 *		Check user privileges on a relation given
 *		name relname and text priv name.
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
	Name		relname = PG_GETARG_NAME(0);
	text	   *priv_type_text = PG_GETARG_TEXT_P(1);
	int32		usesysid;
	bool		result;

	usesysid = GetUserId();

	result = has_table_privilege_id_cname(usesysid,
										  NameStr(*relname),
										  priv_type_text);

	PG_RETURN_BOOL(result);
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
	bool		result;

	result = has_table_privilege_cname_id(NameStr(*username),
										  reloid,
										  priv_type_text);

	PG_RETURN_BOOL(result);
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
	int32		result;

	usesysid = GetUserId();

	/*
	 * Convert priv_type_text to an AclMode
	 */
	mode = convert_priv_string(priv_type_text);

	/*
	 * Check for the privilege
	 */
	result = pg_class_aclcheck(reloid, usesysid, mode);

	if (result == ACLCHECK_OK)
		PG_RETURN_BOOL(true);
	else
		PG_RETURN_BOOL(false);
}


/*
 * has_table_privilege_id_name
 *		Check user privileges on a relation given
 *		usesysid, name relname, and priv name.
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
	Name		relname = PG_GETARG_NAME(1);
	text	   *priv_type_text = PG_GETARG_TEXT_P(2);
	bool		result;

	result = has_table_privilege_id_cname(usesysid,
										  NameStr(*relname),
										  priv_type_text);

	PG_RETURN_BOOL(result);
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
	int32		result;

	/*
	 * Convert priv_type_text to an AclMode
	 */
	mode = convert_priv_string(priv_type_text);

	/*
	 * Check for the privilege
	 */
	result = pg_class_aclcheck(reloid, usesysid, mode);

	if (result == ACLCHECK_OK)
		PG_RETURN_BOOL(true);
	else
		PG_RETURN_BOOL(false);
}

/*
 *		Internal functions.
 */

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
	char	   *priv_type = DatumGetCString(DirectFunctionCall1(textout, PointerGetDatum(priv_type_text)));

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
	return ACL_NO;
}

/*
 * has_table_privilege_cname_cname
 *		Check user privileges on a relation given
 *		char *usename, char *relname, and text priv name.
 *
 * RETURNS
 *		a boolean value
 *		't' indicating user has the privilege
 *		'f' indicating user does not have the privilege
 */
static bool
has_table_privilege_cname_cname(char *username, char *relname,
								text *priv_type_text)
{
	int32		usesysid;

	/*
	 * Lookup userid based on username
	 */
	usesysid = get_usesysid(username);

	/*
	 * Make use of has_table_privilege_id_cname. It accepts the arguments
	 * we now have.
	 */
	return has_table_privilege_id_cname(usesysid, relname, priv_type_text);
}


/*
 * has_table_privilege_cname_id
 *		Check user privileges on a relation given
 *		char *usename, rel oid, and text priv name.
 *
 * RETURNS
 *		a boolean value
 *		't' indicating user has the privilege
 *		'f' indicating user does not have the privilege
 */
static bool
has_table_privilege_cname_id(char *username, Oid reloid,
							 text *priv_type_text)
{
	int32		usesysid;
	char	   *relname;

	/*
	 * Lookup userid based on username
	 */
	usesysid = get_usesysid(username);

	/*
	 * Lookup relname based on rel oid
	 */
	relname = get_rel_name(reloid);
	if (relname == NULL)
		elog(ERROR, "has_table_privilege: invalid relation oid %u",
			 reloid);

	/*
	 * Make use of has_table_privilege_id_cname. It accepts the arguments
	 * we now have.
	 */
	return has_table_privilege_id_cname(usesysid, relname, priv_type_text);
}


/*
 * has_table_privilege_id_cname
 *		Check user privileges on a relation given
 *		usesysid, char *relname, and text priv name.
 *
 * RETURNS
 *		a boolean value
 *		't' indicating user has the privilege
 *		'f' indicating user does not have the privilege
 */
static bool
has_table_privilege_id_cname(int32 usesysid, char *relname,
							 text *priv_type_text)
{
	Oid			reloid;
	AclMode		mode;
	int32		result;

	/*
	 * Convert relname to rel OID.
	 */
	reloid = GetSysCacheOid(RELNAME,
							PointerGetDatum(relname),
							0, 0, 0);
	if (!OidIsValid(reloid))
		elog(ERROR, "has_table_privilege: relation \"%s\" does not exist",
			 relname);

	/*
	 * Convert priv_type_text to an AclMode
	 */
	mode = convert_priv_string(priv_type_text);

	/*
	 * Finally, check for the privilege
	 */
	result = pg_class_aclcheck(reloid, usesysid, mode);

	if (result == ACLCHECK_OK)
		return true;
	else
		return false;
}
