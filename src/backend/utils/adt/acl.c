/*-------------------------------------------------------------------------
 *
 * acl.c
 *	  Basic access control list data structures manipulation routines.
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/adt/acl.c,v 1.55 2000/12/03 20:45:35 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <ctype.h>

#include "postgres.h"

#include "access/heapam.h"
#include "catalog/catalog.h"
#include "catalog/pg_shadow.h"
#include "catalog/pg_type.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/syscache.h"

static char *getid(char *s, char *n);
static bool aclitemeq(AclItem *a1, AclItem *a2);
static bool aclitemgt(AclItem *a1, AclItem *a2);
static char *aclparse(char *s, AclItem *aip, unsigned *modechg);

#define ACL_IDTYPE_GID_KEYWORD	"group"
#define ACL_IDTYPE_UID_KEYWORD	"user"


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
static char *
getid(char *s, char *n)
{
	unsigned	len;
	char	   *id;
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
static char *
aclparse(char *s, AclItem *aip, unsigned *modechg)
{
	HeapTuple	htup;
	char		name[NAMEDATALEN];

	Assert(s && aip && modechg);

#ifdef ACLDEBUG_TRACE
	printf("aclparse: input = '%s'\n", s);
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
			case ACL_MODE_AP_CHR:
				aip->ai_mode |= ACL_AP;
				break;
			case ACL_MODE_RD_CHR:
				aip->ai_mode |= ACL_RD;
				break;
			case ACL_MODE_WR_CHR:
				aip->ai_mode |= ACL_WR;
				break;
			case ACL_MODE_RU_CHR:
				aip->ai_mode |= ACL_RU;
				break;
			default:
				elog(ERROR, "aclparse: mode flags must use \"%s\"",
					 ACL_MODE_STR);
		}
	}

	switch (aip->ai_idtype)
	{
		case ACL_IDTYPE_UID:
			htup = SearchSysCache(SHADOWNAME,
								  PointerGetDatum(name),
								  0, 0, 0);
			if (!HeapTupleIsValid(htup))
				elog(ERROR, "aclparse: non-existent user \"%s\"", name);
			aip->ai_id = ((Form_pg_shadow) GETSTRUCT(htup))->usesysid;
			ReleaseSysCache(htup);
			break;
		case ACL_IDTYPE_GID:
			aip->ai_id = get_grosysid(name);
			break;
		case ACL_IDTYPE_WORLD:
			aip->ai_id = ACL_ID_WORLD;
			break;
	}

#ifdef ACLDEBUG_TRACE
	elog(DEBUG, "aclparse: correctly read [%x %d %x], modechg=%x",
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
	char	   *s = PG_GETARG_CSTRING(0);
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
	AclItem	   *aip = PG_GETARG_ACLITEM_P(0);
	char	   *p;
	char	   *out;
	HeapTuple	htup;
	unsigned	i;
	char	   *tmpname;

	p = out = palloc(strlen("group =arwR ") + 1 + NAMEDATALEN);
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
 *		Two AclItems are equal iff they have the
 *		same identifier (and identifier type).
 *
 * RETURNS:
 *		a boolean value indicating = or >
 */
static bool
aclitemeq(AclItem *a1, AclItem *a2)
{
	return a1->ai_idtype == a2->ai_idtype && a1->ai_id == a2->ai_id;
}

static bool
aclitemgt(AclItem *a1, AclItem *a2)
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
acldefault(char *relname, AclId ownerid)
{
	Acl		   *acl;
	AclItem    *aip;

#define ACL_WORLD_DEFAULT		(ACL_NO)
/* #define		ACL_WORLD_DEFAULT		(ACL_RD|ACL_WR|ACL_AP|ACL_RU) */
#define ACL_OWNER_DEFAULT		(ACL_RD|ACL_WR|ACL_AP|ACL_RU)

	acl = makeacl(2);
	aip = ACL_DAT(acl);
	aip[0].ai_idtype = ACL_IDTYPE_WORLD;
	aip[0].ai_id = ACL_ID_WORLD;
	aip[0].ai_mode = IsSystemRelationName(relname) ? ACL_RD : ACL_WORLD_DEFAULT;
	aip[1].ai_idtype = ACL_IDTYPE_UID;
	aip[1].ai_id = ownerid;
	aip[1].ai_mode = ACL_OWNER_DEFAULT;
	return acl;
}


/*
 * Add or replace an item in an ACL array.
 *
 * NB: caller is responsible for having detoasted the input ACL, if needed.
 */
Acl *
aclinsert3(Acl *old_acl, AclItem *mod_aip, unsigned modechg)
{
	Acl		   *new_acl;
	AclItem    *old_aip,
			   *new_aip;
	int			src,
				dst,
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
		/* modify in-place */
		new_acl = makeacl(num);
		new_aip = ACL_DAT(new_acl);
		memcpy((char *) new_acl, (char *) old_acl, ACL_SIZE(old_acl));
		src = dst;
	}
	else
	{
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
		new_aip[dst].ai_id = mod_aip->ai_id;
		new_aip[dst].ai_idtype = mod_aip->ai_idtype;
		num++;					/* set num to the size of new_acl */
		src = 0;				/* if add or del, start from world entry */
	}

	/* apply the permissions mod */
	switch (modechg)
	{
		case ACL_MODECHG_ADD:
			new_aip[dst].ai_mode = old_aip[src].ai_mode | mod_aip->ai_mode;
			break;
		case ACL_MODECHG_DEL:
			new_aip[dst].ai_mode = old_aip[src].ai_mode & ~mod_aip->ai_mode;
			break;
		case ACL_MODECHG_EQL:
			new_aip[dst].ai_mode = mod_aip->ai_mode;
			break;
	}

	/*
	 * if the adjusted entry has no permissions, delete it from the
	 * list.  For example, this helps in removing entries for users who no
	 * longer exist.  EXCEPTION: never remove the world entry.
	 */
	if (new_aip[dst].ai_mode == 0 && dst > 0)
	{
		int			i;

		for (i = dst + 1; i < num; i++)
		{
			new_aip[i - 1].ai_id = new_aip[i].ai_id;
			new_aip[i - 1].ai_idtype = new_aip[i].ai_idtype;
			new_aip[i - 1].ai_mode = new_aip[i].ai_mode;
		}
		ARR_DIMS(new_acl)[0] = num - 1;
		/* Adjust also the array size because it is used for memcpy */
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
	AclItem	   *mod_aip = PG_GETARG_ACLITEM_P(1);

	PG_RETURN_ACL_P(aclinsert3(old_acl, mod_aip, ACL_MODECHG_EQL));
}

Datum
aclremove(PG_FUNCTION_ARGS)
{
	Acl		   *old_acl = PG_GETARG_ACL_P(0);
	AclItem	   *mod_aip = PG_GETARG_ACLITEM_P(1);
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
	AclItem	   *aip = PG_GETARG_ACLITEM_P(1);
	AclItem    *aidat;
	int			i,
				num;

	num = ACL_NUM(acl);
	aidat = ACL_DAT(acl);
	for (i = 0; i < num; ++i)
		if (aclitemeq(aip, aidat + i))
			PG_RETURN_BOOL(true);
	PG_RETURN_BOOL(false);
}

/*
 * ExecuteChangeACLStmt
 *		Called to execute the utility command type ChangeACLStmt
 */
void
ExecuteChangeACLStmt(ChangeACLStmt *stmt)
{
	AclItem		aclitem;
	unsigned	modechg;
	List	   *i;

	/* see comment in pg_type.h */
	Assert(ACLITEMSIZE == sizeof(AclItem));

	/* Convert string ACL spec into internal form */
	aclparse(stmt->aclString, &aclitem, &modechg);

	foreach(i, stmt->relNames)
	{
		char	   *relname = strVal(lfirst(i));
		Relation	rel;

		rel = heap_openr(relname, AccessExclusiveLock);
		if (rel && rel->rd_rel->relkind == RELKIND_INDEX)
			elog(ERROR, "\"%s\" is an index relation",
				 relname);
		if (!pg_ownercheck(GetUserId(), relname, RELNAME))
			elog(ERROR, "you do not own class \"%s\"",
				 relname);
		ChangeAcl(relname, &aclitem, modechg);
		/* close rel, but keep lock until end of xact */
		heap_close(rel, NoLock);
	}
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
aclmakepriv(char *old_privlist, char new_priv)
{
	char	   *priv;
	int			i;
	int			l;

	Assert(strlen(old_privlist) < 5);
	priv = palloc(5); /* at most "rwaR" */ ;

	if (old_privlist == NULL || old_privlist[0] == '\0')
	{
		priv[0] = new_priv;
		priv[1] = '\0';
		return priv;
	}

	strcpy(priv, old_privlist);

	l = strlen(old_privlist);

	if (l == 4)
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
aclmakeuser(char *user_type, char *user)
{
	char	   *user_list;

	user_list = palloc(strlen(user_type) + strlen(user) + 2);
	sprintf(user_list, "%s %s", user_type, user);
	return user_list;
}

/*
 * makeAclStmt:
 *	  create a ChangeACLStmt at parse time.
 *	  we take in the privileges, relation_name_list, and grantee
 *	  as well as a single character '+' or '-' to indicate grant or revoke
 *
 * We convert the information to the same external form recognized by
 * aclitemin (see aclparse), and save that string in the ChangeACLStmt.
 * Conversion to internal form happens when the statement is executed.
 */
ChangeACLStmt *
makeAclStmt(char *privileges, List *rel_list, char *grantee,
			char grant_or_revoke)
{
	ChangeACLStmt *n = makeNode(ChangeACLStmt);
	StringInfoData str;

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
	n->relNames = rel_list;
	n->aclString = pstrdup(str.data);

	pfree(str.data);
	return n;
}
