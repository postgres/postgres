/*-------------------------------------------------------------------------
 *
 * aclchk.c
 *	  Routines to check access control permissions.
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/catalog/aclchk.c,v 1.46 2001/01/24 19:42:51 momjian Exp $
 *
 * NOTES
 *	  See acl.h.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "catalog/catalog.h"
#include "catalog/catname.h"
#include "catalog/indexing.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_group.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_shadow.h"
#include "catalog/pg_type.h"
#include "miscadmin.h"
#include "parser/parse_agg.h"
#include "parser/parse_func.h"
#include "utils/acl.h"
#include "utils/syscache.h"

static int32 aclcheck(char *relname, Acl *acl, AclId id,
					  AclIdType idtype, AclMode mode);

/* warning messages, now more explicit. */
/* MUST correspond to the order of the ACLCHK_* result codes in acl.h. */
char	   *aclcheck_error_strings[] = {
	"No error.",
	"Permission denied.",
	"Table does not exist.",
	"Must be table owner."
};


#ifdef ACLDEBUG_TRACE
static
dumpacl(Acl *acl)
{
	int			i;
	AclItem    *aip;

	elog(DEBUG, "acl size = %d, # acls = %d",
		 ACL_SIZE(acl), ACL_NUM(acl));
	aip = ACL_DAT(acl);
	for (i = 0; i < ACL_NUM(acl); ++i)
		elog(DEBUG, "	acl[%d]: %s", i,
			 DatumGetCString(DirectFunctionCall1(aclitemout,
												 PointerGetDatum(aip + i))));
}

#endif

/*
 * ChangeAcl
 */
void
ChangeAcl(char *relname,
		  AclItem *mod_aip,
		  unsigned modechg)
{
	unsigned	i;
	Acl		   *old_acl,
			   *new_acl;
	Relation	relation;
	HeapTuple	tuple;
	HeapTuple	newtuple;
	Datum		aclDatum;
	Datum		values[Natts_pg_class];
	char		nulls[Natts_pg_class];
	char		replaces[Natts_pg_class];
	Relation	idescs[Num_pg_class_indices];
	bool		isNull;

	/*
	 * Find the pg_class tuple matching 'relname' and extract the ACL. If
	 * there's no ACL, create a default using the pg_class.relowner field.
	 */
	relation = heap_openr(RelationRelationName, RowExclusiveLock);
	tuple = SearchSysCache(RELNAME,
						   PointerGetDatum(relname),
						   0, 0, 0);
	if (!HeapTupleIsValid(tuple))
	{
		heap_close(relation, RowExclusiveLock);
		elog(ERROR, "ChangeAcl: class \"%s\" not found",
			 relname);
	}

	aclDatum = SysCacheGetAttr(RELNAME, tuple, Anum_pg_class_relacl,
							   &isNull);
	if (isNull)
	{
		/* No ACL, so build default ACL for rel */
		AclId		ownerId;

		ownerId = ((Form_pg_class) GETSTRUCT(tuple))->relowner;
		old_acl = acldefault(relname, ownerId);
	}
	else
	{
		/* get a detoasted copy of the rel's ACL */
		old_acl = DatumGetAclPCopy(aclDatum);
	}

#ifdef ACLDEBUG_TRACE
	dumpacl(old_acl);
#endif

	new_acl = aclinsert3(old_acl, mod_aip, modechg);

#ifdef ACLDEBUG_TRACE
	dumpacl(new_acl);
#endif

	for (i = 0; i < Natts_pg_class; ++i)
	{
		replaces[i] = ' ';
		nulls[i] = ' ';			/* ignored if replaces[i] == ' ' anyway */
		values[i] = (Datum) NULL;		/* ignored if replaces[i] == ' '
										 * anyway */
	}
	replaces[Anum_pg_class_relacl - 1] = 'r';
	values[Anum_pg_class_relacl - 1] = PointerGetDatum(new_acl);
	newtuple = heap_modifytuple(tuple, relation, values, nulls, replaces);

	ReleaseSysCache(tuple);

	simple_heap_update(relation, &newtuple->t_self, newtuple);

	/* keep the catalog indices up to date */
	CatalogOpenIndices(Num_pg_class_indices, Name_pg_class_indices,
					   idescs);
	CatalogIndexInsert(idescs, Num_pg_class_indices, relation, newtuple);
	CatalogCloseIndices(Num_pg_class_indices, idescs);

	heap_close(relation, RowExclusiveLock);

	pfree(old_acl);
	pfree(new_acl);
}

AclId
get_grosysid(char *groname)
{
	HeapTuple	tuple;
	AclId		id = 0;

	tuple = SearchSysCache(GRONAME,
						   PointerGetDatum(groname),
						   0, 0, 0);
	if (HeapTupleIsValid(tuple))
	{
		id = ((Form_pg_group) GETSTRUCT(tuple))->grosysid;
		ReleaseSysCache(tuple);
	}
	else
		elog(ERROR, "non-existent group \"%s\"", groname);
	return id;
}

/*
 * Convert group ID to name, or return NULL if group can't be found
 */
char *
get_groname(AclId grosysid)
{
	HeapTuple	tuple;
	char	   *name = NULL;

	tuple = SearchSysCache(GROSYSID,
						   ObjectIdGetDatum(grosysid),
						   0, 0, 0);
	if (HeapTupleIsValid(tuple))
	{
		name = pstrdup(NameStr(((Form_pg_group) GETSTRUCT(tuple))->groname));
		ReleaseSysCache(tuple);
	}
	return name;
}

static bool
in_group(AclId uid, AclId gid)
{
	bool		result = false;
	HeapTuple	tuple;
	Datum		att;
	bool		isNull;
	IdList	   *tmp;
	AclId	   *aidp;
	int			i,
				num;

	tuple = SearchSysCache(GROSYSID,
						   ObjectIdGetDatum(gid),
						   0, 0, 0);
	if (HeapTupleIsValid(tuple))
	{
		att = SysCacheGetAttr(GROSYSID,
							  tuple,
							  Anum_pg_group_grolist,
							  &isNull);
		if (!isNull)
		{
			/* be sure the IdList is not toasted */
			tmp = DatumGetIdListP(att);
			/* scan it */
			num = IDLIST_NUM(tmp);
			aidp = IDLIST_DAT(tmp);
			for (i = 0; i < num; ++i)
			{
				if (aidp[i] == uid)
				{
					result = true;
					break;
				}
			}
		}
		ReleaseSysCache(tuple);
	}
	else
		elog(NOTICE, "in_group: group %u not found", gid);
	return result;
}

/*
 * aclcheck
 * Returns 1 if the 'id' of type 'idtype' has ACL entries in 'acl' to satisfy
 * any one of the requirements of 'mode'.  Returns 0 otherwise.
 */
static int32
aclcheck(char *relname, Acl *acl, AclId id, AclIdType idtype, AclMode mode)
{
	AclItem    *aip,
			   *aidat;
	int			i,
				num;

	/*
	 * If ACL is null, default to "OK" --- this should not happen,
	 * since caller should have inserted appropriate default
	 */
	if (!acl)
	{
		elog(DEBUG, "aclcheck: null ACL, returning 1");
		return ACLCHECK_OK;
	}

	num = ACL_NUM(acl);
	aidat = ACL_DAT(acl);

	/*
	 * We'll treat the empty ACL like that, too, although this is more
	 * like an error (i.e., you manually blew away your ACL array) -- the
	 * system never creates an empty ACL, since there must always be
	 * a "world" entry in the first slot.
	 */
	if (num < 1)
	{
		elog(DEBUG, "aclcheck: zero-length ACL, returning 1");
		return ACLCHECK_OK;
	}
	Assert(aidat->ai_idtype == ACL_IDTYPE_WORLD);

	switch (idtype)
	{
		case ACL_IDTYPE_UID:
			/* Look for exact match to user */
			for (i = 1, aip = aidat + 1;		/* skip world entry */
				 i < num && aip->ai_idtype == ACL_IDTYPE_UID;
				 ++i, ++aip)
			{
				if (aip->ai_id == id)
				{
#ifdef ACLDEBUG_TRACE
					elog(DEBUG, "aclcheck: found user %u/%d",
						 aip->ai_id, aip->ai_mode);
#endif
					return (aip->ai_mode & mode) ? ACLCHECK_OK : ACLCHECK_NO_PRIV;
				}
			}
			/* See if he has the permission via any group */
			for (;
				 i < num && aip->ai_idtype == ACL_IDTYPE_GID;
				 ++i, ++aip)
			{
				if (aip->ai_mode & mode)
				{
					if (in_group(id, aip->ai_id))
					{
#ifdef ACLDEBUG_TRACE
						elog(DEBUG, "aclcheck: found group %u/%d",
							 aip->ai_id, aip->ai_mode);
#endif
						return ACLCHECK_OK;
					}
				}
			}
			/* Else, look to the world entry */
			break;
		case ACL_IDTYPE_GID:
			/* Look for this group ID */
			for (i = 1, aip = aidat + 1;		/* skip world entry and
												 * UIDs */
				 i < num && aip->ai_idtype == ACL_IDTYPE_UID;
				 ++i, ++aip)
				;
			for (;
				 i < num && aip->ai_idtype == ACL_IDTYPE_GID;
				 ++i, ++aip)
			{
				if (aip->ai_id == id)
				{
#ifdef ACLDEBUG_TRACE
					elog(DEBUG, "aclcheck: found group %u/%d",
						 aip->ai_id, aip->ai_mode);
#endif
					return (aip->ai_mode & mode) ? ACLCHECK_OK : ACLCHECK_NO_PRIV;
				}
			}
			/* Else, look to the world entry */
			break;
		case ACL_IDTYPE_WORLD:
			/* Only check the world entry */
			break;
		default:
			elog(ERROR, "aclcheck: bogus ACL id type: %d", idtype);
			break;
	}

#ifdef ACLDEBUG_TRACE
	elog(DEBUG, "aclcheck: using world=%d", aidat->ai_mode);
#endif
	return (aidat->ai_mode & mode) ? ACLCHECK_OK : ACLCHECK_NO_PRIV;
}

int32
pg_aclcheck(char *relname, Oid userid, AclMode mode)
{
	int32		result;
	HeapTuple	tuple;
	char       *usename;
	Datum		aclDatum;
	bool		isNull;
	Acl		   *acl;

	tuple = SearchSysCache(SHADOWSYSID,
						   ObjectIdGetDatum(userid),
						   0, 0, 0);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "pg_aclcheck: invalid user id %u",
			 (unsigned) userid);

	usename = NameStr(((Form_pg_shadow) GETSTRUCT(tuple))->usename);

	/*
	 * Deny anyone permission to update a system catalog unless
	 * pg_shadow.usecatupd is set.	(This is to let superusers protect
	 * themselves from themselves.)
	 */
	if (((mode & ACL_WR) || (mode & ACL_AP)) &&
		!allowSystemTableMods && IsSystemRelationName(relname) &&
		strncmp(relname, "pg_temp.", strlen("pg_temp.")) != 0 &&
		!((Form_pg_shadow) GETSTRUCT(tuple))->usecatupd)
	{
		elog(DEBUG, "pg_aclcheck: catalog update to \"%s\": permission denied",
			 relname);
		ReleaseSysCache(tuple);
		return ACLCHECK_NO_PRIV;
	}

	/*
	 * Otherwise, superusers bypass all permission-checking.
	 */
	if (((Form_pg_shadow) GETSTRUCT(tuple))->usesuper)
	{
#ifdef ACLDEBUG_TRACE
		elog(DEBUG, "pg_aclcheck: \"%s\" is superuser",
			 usename);
#endif
		ReleaseSysCache(tuple);
		return ACLCHECK_OK;
	}

	ReleaseSysCache(tuple);
	/* caution: usename is inaccessible beyond this point... */

	/*
	 * Normal case: get the relation's ACL from pg_class
	 */
	tuple = SearchSysCache(RELNAME,
						   PointerGetDatum(relname),
						   0, 0, 0);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "pg_aclcheck: class \"%s\" not found", relname);

	aclDatum = SysCacheGetAttr(RELNAME, tuple, Anum_pg_class_relacl,
							   &isNull);
	if (isNull)
	{
		/* No ACL, so build default ACL for rel */
		AclId		ownerId;

		ownerId = ((Form_pg_class) GETSTRUCT(tuple))->relowner;
		acl = acldefault(relname, ownerId);
	}
	else
	{
		/* get a detoasted copy of the rel's ACL */
		acl = DatumGetAclPCopy(aclDatum);
	}

	result = aclcheck(relname, acl, userid, (AclIdType) ACL_IDTYPE_UID, mode);

	if (acl)
		pfree(acl);
	ReleaseSysCache(tuple);

	return result;
}

int32
pg_ownercheck(Oid userid,
			  const char *value,
			  int cacheid)
{
	HeapTuple	tuple;
	AclId		owner_id;
	char       *usename;

	tuple = SearchSysCache(SHADOWSYSID,
						   ObjectIdGetDatum(userid),
						   0, 0, 0);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "pg_ownercheck: invalid user id %u",
			 (unsigned) userid);
	usename = NameStr(((Form_pg_shadow) GETSTRUCT(tuple))->usename);

	/*
	 * Superusers bypass all permission-checking.
	 */
	if (((Form_pg_shadow) GETSTRUCT(tuple))->usesuper)
	{
#ifdef ACLDEBUG_TRACE
		elog(DEBUG, "pg_ownercheck: user \"%s\" is superuser",
			 usename);
#endif
		ReleaseSysCache(tuple);
		return 1;
	}

	ReleaseSysCache(tuple);
	/* caution: usename is inaccessible beyond this point... */

	tuple = SearchSysCache(cacheid,
						   PointerGetDatum(value),
						   0, 0, 0);
	switch (cacheid)
	{
		case OPEROID:
			if (!HeapTupleIsValid(tuple))
				elog(ERROR, "pg_ownercheck: operator %ld not found",
					 PointerGetDatum(value));
			owner_id = ((Form_pg_operator) GETSTRUCT(tuple))->oprowner;
			break;
		case PROCNAME:
			if (!HeapTupleIsValid(tuple))
				elog(ERROR, "pg_ownercheck: function \"%s\" not found",
					 value);
			owner_id = ((Form_pg_proc) GETSTRUCT(tuple))->proowner;
			break;
		case RELNAME:
			if (!HeapTupleIsValid(tuple))
				elog(ERROR, "pg_ownercheck: class \"%s\" not found",
					 value);
			owner_id = ((Form_pg_class) GETSTRUCT(tuple))->relowner;
			break;
		case TYPENAME:
			if (!HeapTupleIsValid(tuple))
				elog(ERROR, "pg_ownercheck: type \"%s\" not found",
					 value);
			owner_id = ((Form_pg_type) GETSTRUCT(tuple))->typowner;
			break;
		default:
			elog(ERROR, "pg_ownercheck: invalid cache id: %d", cacheid);
			owner_id = 0;		/* keep compiler quiet */
			break;
	}

	ReleaseSysCache(tuple);

	return userid == owner_id;
}

int32
pg_func_ownercheck(Oid userid,
				   char *funcname,
				   int nargs,
				   Oid *arglist)
{
	HeapTuple	tuple;
	AclId		owner_id;
	char	   *usename;

	tuple = SearchSysCache(SHADOWSYSID,
						   ObjectIdGetDatum(userid),
						   0, 0, 0);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "pg_func_ownercheck: invalid user id %u",
			 (unsigned) userid);
	usename = NameStr(((Form_pg_shadow) GETSTRUCT(tuple))->usename);

	/*
	 * Superusers bypass all permission-checking.
	 */
	if (((Form_pg_shadow) GETSTRUCT(tuple))->usesuper)
	{
#ifdef ACLDEBUG_TRACE
		elog(DEBUG, "pg_ownercheck: user \"%s\" is superuser",
			 usename);
#endif
		ReleaseSysCache(tuple);
		return 1;
	}

	ReleaseSysCache(tuple);
	/* caution: usename is inaccessible beyond this point... */

	tuple = SearchSysCache(PROCNAME,
						   PointerGetDatum(funcname),
						   Int32GetDatum(nargs),
						   PointerGetDatum(arglist),
						   0);
	if (!HeapTupleIsValid(tuple))
		func_error("pg_func_ownercheck", funcname, nargs, arglist, NULL);

	owner_id = ((Form_pg_proc) GETSTRUCT(tuple))->proowner;

	ReleaseSysCache(tuple);

	return userid == owner_id;
}

int32
pg_aggr_ownercheck(Oid userid,
				   char *aggname,
				   Oid basetypeID)
{
	HeapTuple	tuple;
	AclId		owner_id;
	char	   *usename;

	tuple = SearchSysCache(SHADOWSYSID,
						   PointerGetDatum(userid),
						   0, 0, 0);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "pg_aggr_ownercheck: invalid user id %u",
			 (unsigned) userid);
	usename = NameStr(((Form_pg_shadow) GETSTRUCT(tuple))->usename);

	/*
	 * Superusers bypass all permission-checking.
	 */
	if (((Form_pg_shadow) GETSTRUCT(tuple))->usesuper)
	{
#ifdef ACLDEBUG_TRACE
		elog(DEBUG, "pg_aggr_ownercheck: user \"%s\" is superuser",
			 usename);
#endif
		ReleaseSysCache(tuple);
		return 1;
	}

	ReleaseSysCache(tuple);
	/* caution: usename is inaccessible beyond this point... */

	tuple = SearchSysCache(AGGNAME,
						   PointerGetDatum(aggname),
						   ObjectIdGetDatum(basetypeID),
						   0, 0);
	if (!HeapTupleIsValid(tuple))
		agg_error("pg_aggr_ownercheck", aggname, basetypeID);

	owner_id = ((Form_pg_aggregate) GETSTRUCT(tuple))->aggowner;

	ReleaseSysCache(tuple);

	return userid == owner_id;
}
