/*-------------------------------------------------------------------------
 *
 * acl.c
 *	  Basic access control list data structures manipulation routines.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/adt/acl.c,v 1.33 1999/02/13 23:18:58 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <ctype.h>
#include <string.h>
#include "postgres.h"

#include <utils/memutils.h>
#include "utils/acl.h"
#include "utils/syscache.h"
#include "catalog/catalog.h"
#include "catalog/pg_shadow.h"
#include "miscadmin.h"

static char *getid(char *s, char *n);
static int32 aclitemeq(AclItem *a1, AclItem *a2);
static int32 aclitemgt(AclItem *a1, AclItem *a2);
static char *aclparse(char *s, AclItem *aip, unsigned *modechg);

#define ACL_IDTYPE_GID_KEYWORD	"group"
#define ACL_IDTYPE_UID_KEYWORD	"user"


/*
 * getid
 *		Consumes the first alphanumeric string (identifier) found in string
 *		's', ignoring any leading white space.
 *
 * RETURNS:
 *		the string position in 's' that points to the next non-space character
 *		in 's'.  Also:
 *		- loads the identifier into 'name'.  (If no identifier is found, 'name'
 *		  contains an empty string).
 */
static char *
getid(char *s, char *n)
{
	unsigned	len;
	char	   *id;

	Assert(s && n);

	while (isspace(*s))
		++s;
	for (id = s, len = 0; isalnum(*s) || *s == '_'; ++len, ++s)
		;
	if (len > sizeof(NameData))
		elog(ERROR, "getid: identifier cannot be >%d characters",
			 sizeof(NameData));
	if (len > 0)
		memmove(n, id, len);
	n[len] = '\0';
	while (isspace(*s))
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

	aip->ai_idtype = ACL_IDTYPE_UID;
	s = getid(s, name);
	if (*s != ACL_MODECHG_ADD_CHR &&
		*s != ACL_MODECHG_DEL_CHR &&
		*s != ACL_MODECHG_EQL_CHR)
	{							/* we just read a keyword, not a name */
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
	while (isalpha(*++s))
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
			htup = SearchSysCacheTuple(USENAME,
									   PointerGetDatum(name),
									   0, 0, 0);
			if (!HeapTupleIsValid(htup))
				elog(ERROR, "aclparse: non-existent user \"%s\"", name);
			aip->ai_id = ((Form_pg_shadow) GETSTRUCT(htup))->usesysid;
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
		elog(ERROR, "makeacl: invalid size: %d\n", n);
	size = ACL_N_SIZE(n);
	if (!(new_acl = (Acl *) palloc(size)))
		elog(ERROR, "makeacl: palloc failed on %d\n", size);
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
AclItem    *
aclitemin(char *s)
{
	unsigned	modechg;
	AclItem    *aip;

	if (!s)
		elog(ERROR, "aclitemin: null string");

	aip = (AclItem *) palloc(sizeof(AclItem));
	if (!aip)
		elog(ERROR, "aclitemin: palloc failed");
	s = aclparse(s, aip, &modechg);
	if (modechg != ACL_MODECHG_EQL)
		elog(ERROR, "aclitemin: cannot accept anything but = ACLs");
	while (isspace(*s))
		++s;
	if (*s)
		elog(ERROR, "aclitemin: extra garbage at end of specification");
	return aip;
}

/*
 * aclitemout
 *		Allocates storage for, and fills in, a new null-delimited string
 *		containing a formatted ACL specification.  See aclparse for details.
 *
 * RETURNS:
 *		the new string
 */
char *
aclitemout(AclItem *aip)
{
	char	   *p;
	char	   *out;
	HeapTuple	htup;
	unsigned	i;
	static AclItem default_aclitem = {ACL_ID_WORLD,
		ACL_IDTYPE_WORLD,
	ACL_WORLD_DEFAULT};
	extern char *int2out();
	char	   *tmpname;

	if (!aip)
		aip = &default_aclitem;

	p = out = palloc(strlen("group =arwR ") + 1 + NAMEDATALEN);
	if (!out)
		elog(ERROR, "aclitemout: palloc failed");
	*p = '\0';

	switch (aip->ai_idtype)
	{
		case ACL_IDTYPE_UID:
			htup = SearchSysCacheTuple(USESYSID,
									   ObjectIdGetDatum(aip->ai_id),
									   0, 0, 0);
			if (!HeapTupleIsValid(htup))
			{
				char	   *tmp = int2out(aip->ai_id);

#ifdef NOT_USED

				When this	elog(NOTICE) goes to the libpq client,
							it crashes the
							client because the NOTICE protocol is coming right in the middle
							of a request for a field value.We skip the NOTICE for now.

							elog(NOTICE, "aclitemout: usesysid %d not found",
											 aip->ai_id);

#endif

				strcat(p, tmp);
				pfree(tmp);
			}
			else
				strncat(p, (char *) &((Form_pg_shadow)
									  GETSTRUCT(htup))->usename,
						sizeof(NameData));
			break;
		case ACL_IDTYPE_GID:
			strcat(p, "group ");
			tmpname = get_groname(aip->ai_id);
			strncat(p, tmpname, NAMEDATALEN);
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

	return out;
}

/*
 * aclitemeq
 * aclitemgt
 *		AclItem equality and greater-than comparison routines.
 *		Two AclItems are equal iff they are both NULL or they have the
 *		same identifier (and identifier type).
 *
 * RETURNS:
 *		a boolean value indicating = or >
 */
static int32
aclitemeq(AclItem *a1, AclItem *a2)
{
	if (!a1 && !a2)
		return 1;
	if (!a1 || !a2)
		return 0;
	return a1->ai_idtype == a2->ai_idtype && a1->ai_id == a2->ai_id;
}

static int32
aclitemgt(AclItem *a1, AclItem *a2)
{
	if (a1 && !a2)
		return 1;
	if (!a1 || !a2)
		return 0;
	return ((a1->ai_idtype > a2->ai_idtype) ||
			(a1->ai_idtype == a2->ai_idtype && a1->ai_id > a2->ai_id));
}

Acl *
aclownerdefault(char *relname, AclId ownerid)
{
	Acl		   *acl;
	AclItem    *aip;

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

Acl *
acldefault(char *relname)
{
	Acl		   *acl;
	AclItem    *aip;

	acl = makeacl(1);
	aip = ACL_DAT(acl);
	aip[0].ai_idtype = ACL_IDTYPE_WORLD;
	aip[0].ai_id = ACL_ID_WORLD;
	aip[0].ai_mode = IsSystemRelationName(relname) ? ACL_RD : ACL_WORLD_DEFAULT;
	return acl;
}

Acl *
aclinsert3(Acl *old_acl, AclItem *mod_aip, unsigned modechg)
{
	Acl		   *new_acl;
	AclItem    *old_aip,
			   *new_aip;
	unsigned	src,
				dst,
				num;

	if (!old_acl || ACL_NUM(old_acl) < 1)
	{
		new_acl = makeacl(0);
		return new_acl;
	}
	if (!mod_aip)
	{
		new_acl = makeacl(ACL_NUM(old_acl));
		memmove((char *) new_acl, (char *) old_acl, ACL_SIZE(old_acl));
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
		new_acl = makeacl(ACL_NUM(old_acl));
		memmove((char *) new_acl, (char *) old_acl, ACL_SIZE(old_acl));
		new_aip = ACL_DAT(new_acl);
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
			memmove((char *) new_aip,
					(char *) old_aip,
					num * sizeof(AclItem));
		}
		else
		{						/* middle */
			memmove((char *) new_aip,
					(char *) old_aip,
					dst * sizeof(AclItem));
			memmove((char *) (new_aip + dst + 1),
					(char *) (old_aip + dst),
					(num - dst) * sizeof(AclItem));
		}
		new_aip[dst].ai_id = mod_aip->ai_id;
		new_aip[dst].ai_idtype = mod_aip->ai_idtype;
		num++;					/* set num to the size of new_acl */
		src = 0;				/* world entry */
	}
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
	 * if the newly added entry has no permissions, delete it from the
	 * list.  For example, this helps in removing entries for users who no
	 * longer exists...
	 */
	for (dst = 1; dst < num; dst++)
	{
		if (new_aip[dst].ai_mode == 0)
		{
			int			i;

			for (i = dst + 1; i < num; i++)
			{
				new_aip[i - 1].ai_id = new_aip[i].ai_id;
				new_aip[i - 1].ai_idtype = new_aip[i].ai_idtype;
				new_aip[i - 1].ai_mode = new_aip[i].ai_mode;
			}
			ARR_DIMS(new_acl)[0] = num - 1;
			/* Adjust also the array size because it is used for memmove */
			ARR_SIZE(new_acl) -= sizeof(AclItem);
			break;
		}
	}

	return new_acl;
}

/*
 * aclinsert
 *
 */
Acl *
aclinsert(Acl *old_acl, AclItem *mod_aip)
{
	return aclinsert3(old_acl, mod_aip, ACL_MODECHG_EQL);
}

Acl *
aclremove(Acl *old_acl, AclItem *mod_aip)
{
	Acl		   *new_acl;
	AclItem    *old_aip,
			   *new_aip;
	unsigned	dst,
				old_num,
				new_num;

	if (!old_acl || ACL_NUM(old_acl) < 1)
	{
		new_acl = makeacl(0);
		return new_acl;
	}
	if (!mod_aip)
	{
		new_acl = makeacl(ACL_NUM(old_acl));
		memmove((char *) new_acl, (char *) old_acl, ACL_SIZE(old_acl));
		return new_acl;
	}

	old_num = ACL_NUM(old_acl);
	old_aip = ACL_DAT(old_acl);

	for (dst = 0; dst < old_num && !aclitemeq(mod_aip, old_aip + dst); ++dst)
		;
	if (dst >= old_num)
	{							/* not found or empty */
		new_acl = makeacl(ACL_NUM(old_acl));
		memmove((char *) new_acl, (char *) old_acl, ACL_SIZE(old_acl));
	}
	else
	{
		new_num = old_num - 1;
		new_acl = makeacl(ACL_NUM(old_acl) - 1);
		new_aip = ACL_DAT(new_acl);
		if (dst == 0)
		{						/* start */
			elog(ERROR, "aclremove: removal of the world ACL??");
		}
		else if (dst == old_num - 1)
		{						/* end */
			memmove((char *) new_aip,
					(char *) old_aip,
					new_num * sizeof(AclItem));
		}
		else
		{						/* middle */
			memmove((char *) new_aip,
					(char *) old_aip,
					dst * sizeof(AclItem));
			memmove((char *) (new_aip + dst),
					(char *) (old_aip + dst + 1),
					(new_num - dst) * sizeof(AclItem));
		}
	}
	return new_acl;
}

int32
aclcontains(Acl *acl, AclItem *aip)
{
	unsigned	i,
				num;
	AclItem    *aidat;

	if (!acl || !aip || ((num = ACL_NUM(acl)) < 1))
		return 0;
	aidat = ACL_DAT(acl);
	for (i = 0; i < num; ++i)
		if (aclitemeq(aip, aidat + i))
			return 1;
	return 0;
}

/* parser support routines */

/*
 * aclmakepriv
 *	  make a acl privilege string out of an existing privilege string
 * and a new privilege
 *
 * does not add duplicate privileges
 *
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
 * concatentates the two strings together with a space in between
 *
 * this routine is used in the parser
 *
 */

char *
aclmakeuser(char *user_type, char *user)
{
	char	   *user_list;

	user_list = palloc(strlen(user) + 3);
	sprintf(user_list, "%s %s", user_type, user);
	return user_list;
}


/*
 * makeAclStmt:
 *	  this is a helper routine called by the parser
 * create a ChangeAclStmt
 *	  we take in the privilegs, relation_name_list, and grantee
 * as well as a single character '+' or '-' to indicate grant or revoke
 *
 * returns a new ChangeACLStmt*
 *
 * this routines works by creating a old-style changle acl string and
 * then calling aclparse;
 */

ChangeACLStmt *
makeAclStmt(char *privileges, List *rel_list, char *grantee,
			char grant_or_revoke)
{
	ChangeACLStmt *n = makeNode(ChangeACLStmt);
	char		str[MAX_PARSE_BUFFER];

	n->aclitem = (AclItem *) palloc(sizeof(AclItem));

	/* the grantee string is "G <group_name>", "U  <user_name>", or "ALL" */
	if (grantee[0] == 'G')		/* group permissions */
	{
		sprintf(str, "%s %s%c%s",
				ACL_IDTYPE_GID_KEYWORD,
				grantee + 2, grant_or_revoke, privileges);
	}
	else if (grantee[0] == 'U') /* user permission */
	{
		sprintf(str, "%s %s%c%s",
				ACL_IDTYPE_UID_KEYWORD,
				grantee + 2, grant_or_revoke, privileges);
	}
	else
/* all permission */
	{
		sprintf(str, "%c%s",
				grant_or_revoke, privileges);
	}
	n->relNames = rel_list;
	aclparse(str, n->aclitem, (unsigned *) &n->modechg);
	return n;
}
