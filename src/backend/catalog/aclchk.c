/*-------------------------------------------------------------------------
 *
 * aclchk.c
 *	  Routines to check access control permissions.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/catalog/aclchk.c,v 1.20 1999/03/17 22:52:47 momjian Exp $
 *
 * NOTES
 *	  See acl.h.
 *
 *-------------------------------------------------------------------------
 */
#include <string.h>
#include "postgres.h"

#include "utils/acl.h"			/* where declarations for this file go */
#include "access/heapam.h"
#include "access/htup.h"
#include "access/tupmacs.h"
#include "catalog/indexing.h"
#include "catalog/catalog.h"
#include "catalog/catname.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_group.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_shadow.h"
#include "catalog/pg_type.h"
#include "fmgr.h"
#include "parser/parse_agg.h"
#include "parser/parse_func.h"
#include "storage/bufmgr.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/syscache.h"
#include "utils/tqual.h"
#include "miscadmin.h"

static int32 aclcheck(char *relname, Acl *acl, AclId id, AclIdType idtype, AclMode mode);

/*
 * Enable use of user relations in place of real system catalogs.
 */
/*#define ACLDEBUG*/

#ifdef ACLDEBUG
/*
 * Fool the code below into thinking that "pgacls" is pg_class.
 * relname and relowner are in the same place, happily.
 */
#undef	Anum_pg_class_relacl
#define Anum_pg_class_relacl			3
#undef	Natts_pg_class
#define Natts_pg_class					3
#undef	Name_pg_class
#define Name_pg_class					"pgacls"
#undef	Name_pg_group
#define Name_pg_group					"pggroup"
#endif

/* warning messages, now more explicit. */
/* should correspond to the order of the ACLCHK_* result codes above. */
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
	unsigned	i;
	AclItem    *aip;

	elog(DEBUG, "acl size = %d, # acls = %d",
		 ACL_SIZE(acl), ACL_NUM(acl));
	aip = (AclItem *) ACL_DAT(acl);
	for (i = 0; i < ACL_NUM(acl); ++i)
		elog(DEBUG, "	acl[%d]: %s", i, aclitemout(aip + i));
}

#endif

/*
 *
 */
void
ChangeAcl(char *relname,
		  AclItem *mod_aip,
		  unsigned modechg)
{
	unsigned	i;
	Acl		   *old_acl = (Acl *) NULL,
			   *new_acl;
	Relation	relation;
	HeapTuple	tuple;
	Datum		values[Natts_pg_class];
	char		nulls[Natts_pg_class];
	char		replaces[Natts_pg_class];
	Relation	idescs[Num_pg_class_indices];
	int			free_old_acl = 0;

	/*
	 * Find the pg_class tuple matching 'relname' and extract the ACL. If
	 * there's no ACL, create a default using the pg_class.relowner field.
	 *
	 * We can't use the syscache here, since we need to do a heap_replace on
	 * the tuple we find.
	 */
	relation = heap_openr(RelationRelationName);
	if (!RelationIsValid(relation))
		elog(ERROR, "ChangeAcl: could not open '%s'??",
			 RelationRelationName);
	tuple = SearchSysCacheTuple(RELNAME,
								PointerGetDatum(relname),
								0, 0, 0);
	if (!HeapTupleIsValid(tuple))
	{
		heap_close(relation);
		elog(ERROR, "ChangeAcl: class \"%s\" not found",
			 relname);
		return;
	}

	if (!heap_attisnull(tuple, Anum_pg_class_relacl))
		old_acl = (Acl *) heap_getattr(tuple,
									   Anum_pg_class_relacl,
									   RelationGetDescr(relation),
									   (bool *) NULL);
	if (!old_acl || ACL_NUM(old_acl) < 1)
	{
#ifdef ACLDEBUG_TRACE
		elog(DEBUG, "ChangeAcl: using default ACL");
#endif
/*		old_acl = acldefault(((Form_pg_class) GETSTRUCT(tuple))->relowner); */
		old_acl = acldefault(relname);
		free_old_acl = 1;
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
	values[Anum_pg_class_relacl - 1] = (Datum) new_acl;
	tuple = heap_modifytuple(tuple, relation, values, nulls, replaces);
	/* XXX handle index on pg_class? */
	setheapoverride(true);
	heap_replace(relation, &tuple->t_self, tuple, NULL);
	setheapoverride(false);

	/* keep the catalog indices up to date */
	CatalogOpenIndices(Num_pg_class_indices, Name_pg_class_indices,
					   idescs);
	CatalogIndexInsert(idescs, Num_pg_class_indices, relation, tuple);
	CatalogCloseIndices(Num_pg_class_indices, idescs);

	heap_close(relation);
	if (free_old_acl)
		pfree(old_acl);
	pfree(new_acl);
}

AclId
get_grosysid(char *groname)
{
	HeapTuple	tuple;
	AclId		id = 0;

	tuple = SearchSysCacheTuple(GRONAME,
								PointerGetDatum(groname),
								0, 0, 0);
	if (HeapTupleIsValid(tuple))
		id = ((Form_pg_group) GETSTRUCT(tuple))->grosysid;
	else
		elog(ERROR, "non-existent group \"%s\"", groname);
	return id;
}

char *
get_groname(AclId grosysid)
{
	HeapTuple	tuple;
	char	   *name = NULL;

	tuple = SearchSysCacheTuple(GROSYSID,
								ObjectIdGetDatum(grosysid),
								0, 0, 0);
	if (HeapTupleIsValid(tuple))
		name = (((Form_pg_group) GETSTRUCT(tuple))->groname).data;
	else
		elog(NOTICE, "get_groname: group %d not found", grosysid);
	return name;
}

static int32
in_group(AclId uid, AclId gid)
{
	Relation	relation;
	HeapTuple	tuple;
	Acl		   *tmp;
	unsigned	i,
				num;
	AclId	   *aidp;
	int32		found = 0;

	relation = heap_openr(GroupRelationName);
	if (!RelationIsValid(relation))
	{
		elog(NOTICE, "in_group: could not open \"%s\"??",
			 GroupRelationName);
		return 0;
	}
	tuple = SearchSysCacheTuple(GROSYSID,
								ObjectIdGetDatum(gid),
								0, 0, 0);
	if (HeapTupleIsValid(tuple) &&
		!heap_attisnull(tuple, Anum_pg_group_grolist))
	{
		tmp = (IdList *) heap_getattr(tuple,
									  Anum_pg_group_grolist,
									  RelationGetDescr(relation),
									  (bool *) NULL);
		/* XXX make me a function */
		num = IDLIST_NUM(tmp);
		aidp = IDLIST_DAT(tmp);
		for (i = 0; i < num; ++i)
			if (aidp[i] == uid)
			{
				found = 1;
				break;
			}
	}
	else
		elog(NOTICE, "in_group: group %d not found", gid);
	heap_close(relation);
	return found;
}

/*
 * aclcheck
 * Returns 1 if the 'id' of type 'idtype' has ACL entries in 'acl' to satisfy
 * any one of the requirements of 'mode'.  Returns 0 otherwise.
 */
static int32
aclcheck(char *relname, Acl *acl, AclId id, AclIdType idtype, AclMode mode)
{
	unsigned	i;
	AclItem    *aip,
			   *aidat;
	unsigned	num,
				found_group;

	/* if no acl is found, use world default */
	if (!acl)
		acl = acldefault(relname);

	num = ACL_NUM(acl);
	aidat = ACL_DAT(acl);

	/*
	 * We'll treat the empty ACL like that, too, although this is more
	 * like an error (i.e., you manually blew away your ACL array) -- the
	 * system never creates an empty ACL.
	 */
	if (num < 1)
	{
#if ACLDEBUG_TRACE || 1
		elog(DEBUG, "aclcheck: zero-length ACL, returning 1");
#endif
		return ACLCHECK_OK;
	}

	switch (idtype)
	{
		case ACL_IDTYPE_UID:
			for (i = 1, aip = aidat + 1;		/* skip world entry */
				 i < num && aip->ai_idtype == ACL_IDTYPE_UID;
				 ++i, ++aip)
			{
				if (aip->ai_id == id)
				{
#ifdef ACLDEBUG_TRACE
					elog(DEBUG, "aclcheck: found %d/%d",
						 aip->ai_id, aip->ai_mode);
#endif
					return (aip->ai_mode & mode) ? ACLCHECK_OK : ACLCHECK_NO_PRIV;
				}
			}
			for (found_group = 0;
				 i < num && aip->ai_idtype == ACL_IDTYPE_GID;
				 ++i, ++aip)
			{
				if (in_group(id, aip->ai_id))
				{
					if (aip->ai_mode & mode)
					{
						found_group = 1;
						break;
					}
				}
			}
			if (found_group)
			{
#ifdef ACLDEBUG_TRACE
				elog(DEBUG, "aclcheck: all groups ok");
#endif
				return ACLCHECK_OK;
			}
			break;
		case ACL_IDTYPE_GID:
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
					elog(DEBUG, "aclcheck: found %d/%d",
						 aip->ai_id, aip->ai_mode);
#endif
					return (aip->ai_mode & mode) ? ACLCHECK_OK : ACLCHECK_NO_PRIV;
				}
			}
			break;
		case ACL_IDTYPE_WORLD:
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
pg_aclcheck(char *relname, char *usename, AclMode mode)
{
	HeapTuple	tuple;
	AclId		id;
	Acl		   *acl = (Acl *) NULL,
			   *tmp;
	int32		result;
	Relation	relation;

	tuple = SearchSysCacheTuple(USENAME,
								PointerGetDatum(usename),
								0, 0, 0);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "pg_aclcheck: user \"%s\" not found",
			 usename);
	id = (AclId) ((Form_pg_shadow) GETSTRUCT(tuple))->usesysid;

	/*
	 * for the 'pg_database' relation, check the usecreatedb field before
	 * checking normal permissions
	 */
	if (strcmp(DatabaseRelationName, relname) == 0 &&
		(((Form_pg_shadow) GETSTRUCT(tuple))->usecreatedb))
	{

		/*
		 * note that even though the user can now append to the
		 * pg_database table, there is still additional permissions
		 * checking in dbcommands.c
		 */
		if ((mode & ACL_WR) || (mode & ACL_AP))
			return ACLCHECK_OK;
	}

	/*
	 * Deny anyone permission to update a system catalog unless
	 * pg_shadow.usecatupd is set.	(This is to let superusers protect
	 * themselves from themselves.)
	 */
	if (((mode & ACL_WR) || (mode & ACL_AP)) &&
		!allowSystemTableMods && IsSystemRelationName(relname) &&
		!((Form_pg_shadow) GETSTRUCT(tuple))->usecatupd)
	{
		elog(DEBUG, "pg_aclcheck: catalog update to \"%s\": permission denied",
			 relname);
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
		return ACLCHECK_OK;
	}

#ifndef ACLDEBUG
	tuple = SearchSysCacheTuple(RELNAME,
								PointerGetDatum(relname),
								0, 0, 0);
	if (!HeapTupleIsValid(tuple))
	{
		elog(ERROR, "pg_aclcheck: class \"%s\" not found",
			 relname);
		/* an elog(ERROR) kills us, so no need to return anything. */
	}
	if (!heap_attisnull(tuple, Anum_pg_class_relacl))
	{
		relation = heap_openr(RelationRelationName);
		tmp = (Acl *) heap_getattr(tuple,
								   Anum_pg_class_relacl,
								   RelationGetDescr(relation),
								   (bool *) NULL);
		acl = makeacl(ACL_NUM(tmp));
		memmove((char *) acl, (char *) tmp, ACL_SIZE(tmp));
		heap_close(relation);
	}
	else
	{

		/*
		 * if the acl is null, by default the owner can do whatever he
		 * wants to with it
		 */
		int4		ownerId;

		relation = heap_openr(RelationRelationName);
		ownerId = (int4) heap_getattr(tuple,
									  Anum_pg_class_relowner,
									  RelationGetDescr(relation),
									  (bool *) NULL);
		acl = aclownerdefault(relname, (AclId) ownerId);
	}
#else
	{							/* This is why the syscache is great... */
		static ScanKeyData relkey[1] = {
			{0, Anum_pg_class_relname, F_NAMEEQ}
		};

		relation = heap_openr(RelationRelationName);
		if (!RelationIsValid(relation))
		{
			elog(NOTICE, "pg_checkacl: could not open \"%-.*s\"??",
				 RelationRelationName);
			return ACLCHECK_NO_CLASS;
		}
		tuple = SearchSysCacheTuple(RELNAME,
									PointerGetDatum(relname),
									0, 0, 0);
		if (HeapTupleIsValid(tuple) &&
			!heap_attisnull(tuple, Anum_pg_class_relacl))
		{
			tmp = (Acl *) heap_getattr(tuple,
									   Anum_pg_class_relacl,
									   RelationGetDescr(relation),
									   (bool *) NULL);
			acl = makeacl(ACL_NUM(tmp));
			memmove((char *) acl, (char *) tmp, ACL_SIZE(tmp));
		}
		heap_close(relation);
	}
#endif
	result = aclcheck(relname, acl, id, (AclIdType) ACL_IDTYPE_UID, mode);
	if (acl)
		pfree(acl);
	return result;
}

int32
pg_ownercheck(char *usename,
			  char *value,
			  int cacheid)
{
	HeapTuple	tuple;
	AclId		user_id,
				owner_id = 0;

	tuple = SearchSysCacheTuple(USENAME,
								PointerGetDatum(usename),
								0, 0, 0);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "pg_ownercheck: user \"%s\" not found",
			 usename);
	user_id = (AclId) ((Form_pg_shadow) GETSTRUCT(tuple))->usesysid;

	/*
	 * Superusers bypass all permission-checking.
	 */
	if (((Form_pg_shadow) GETSTRUCT(tuple))->usesuper)
	{
#ifdef ACLDEBUG_TRACE
		elog(DEBUG, "pg_ownercheck: user \"%s\" is superuser",
			 usename);
#endif
		return 1;
	}

	tuple = SearchSysCacheTuple(cacheid, PointerGetDatum(value),
								0, 0, 0);
	switch (cacheid)
	{
		case OPROID:
			if (!HeapTupleIsValid(tuple))
				elog(ERROR, "pg_ownercheck: operator %ld not found",
					 PointerGetDatum(value));
			owner_id = ((Form_pg_operator) GETSTRUCT(tuple))->oprowner;
			break;
		case PRONAME:
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
		case TYPNAME:
			if (!HeapTupleIsValid(tuple))
				elog(ERROR, "pg_ownercheck: type \"%s\" not found",
					 value);
			owner_id = ((Form_pg_type) GETSTRUCT(tuple))->typowner;
			break;
		default:
			elog(ERROR, "pg_ownercheck: invalid cache id: %d",
				 cacheid);
			break;
	}

	return user_id == owner_id;
}

int32
pg_func_ownercheck(char *usename,
				   char *funcname,
				   int nargs,
				   Oid *arglist)
{
	HeapTuple	tuple;
	AclId		user_id,
				owner_id;

	tuple = SearchSysCacheTuple(USENAME,
								PointerGetDatum(usename),
								0, 0, 0);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "pg_func_ownercheck: user \"%s\" not found",
			 usename);
	user_id = (AclId) ((Form_pg_shadow) GETSTRUCT(tuple))->usesysid;

	/*
	 * Superusers bypass all permission-checking.
	 */
	if (((Form_pg_shadow) GETSTRUCT(tuple))->usesuper)
	{
#ifdef ACLDEBUG_TRACE
		elog(DEBUG, "pg_ownercheck: user \"%s\" is superuser",
			 usename);
#endif
		return 1;
	}

	tuple = SearchSysCacheTuple(PRONAME,
								PointerGetDatum(funcname),
								Int32GetDatum(nargs),
								PointerGetDatum(arglist),
								0);
	if (!HeapTupleIsValid(tuple))
		func_error("pg_func_ownercheck", funcname, nargs, arglist, NULL);

	owner_id = ((Form_pg_proc) GETSTRUCT(tuple))->proowner;

	return user_id == owner_id;
}

int32
pg_aggr_ownercheck(char *usename,
				   char *aggname,
				   Oid basetypeID)
{
	HeapTuple	tuple;
	AclId		user_id,
				owner_id;

	tuple = SearchSysCacheTuple(USENAME,
								PointerGetDatum(usename),
								0, 0, 0);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "pg_aggr_ownercheck: user \"%s\" not found",
			 usename);
	user_id = (AclId) ((Form_pg_shadow) GETSTRUCT(tuple))->usesysid;

	/*
	 * Superusers bypass all permission-checking.
	 */
	if (((Form_pg_shadow) GETSTRUCT(tuple))->usesuper)
	{
#ifdef ACLDEBUG_TRACE
		elog(DEBUG, "pg_aggr_ownercheck: user \"%s\" is superuser",
			 usename);
#endif
		return 1;
	}

	tuple = SearchSysCacheTuple(AGGNAME,
								PointerGetDatum(aggname),
								ObjectIdGetDatum(basetypeID),
								0, 0);

	if (!HeapTupleIsValid(tuple))
		agg_error("pg_aggr_ownercheck", aggname, basetypeID);

	owner_id = ((Form_pg_aggregate) GETSTRUCT(tuple))->aggowner;

	return user_id == owner_id;
}
