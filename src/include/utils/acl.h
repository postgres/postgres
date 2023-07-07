/*-------------------------------------------------------------------------
 *
 * acl.h
 *	  Definition of (and support for) access control list data structures.
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/acl.h
 *
 * NOTES
 *	  An ACL array is simply an array of AclItems, representing the union
 *	  of the privileges represented by the individual items.  A zero-length
 *	  array represents "no privileges".
 *
 *	  The order of items in the array is important as client utilities (in
 *	  particular, pg_dump, though possibly other clients) expect to be able
 *	  to issue GRANTs in the ordering of the items in the array.  The reason
 *	  this matters is that GRANTs WITH GRANT OPTION must be before any GRANTs
 *	  which depend on it.  This happens naturally in the backend during
 *	  operations as we update ACLs in-place, new items are appended, and
 *	  existing entries are only removed if there's no dependency on them (no
 *	  GRANT can been based on it, or, if there was, those GRANTs are also
 *	  removed).
 *
 *	  For backward-compatibility purposes we have to allow null ACL entries
 *	  in system catalogs.  A null ACL will be treated as meaning "default
 *	  protection" (i.e., whatever acldefault() returns).
 *-------------------------------------------------------------------------
 */
#ifndef ACL_H
#define ACL_H

#include "access/htup.h"
#include "nodes/parsenodes.h"
#include "parser/parse_node.h"
#include "utils/snapshot.h"


/*
 * typedef AclMode is declared in parsenodes.h, also the individual privilege
 * bit meanings are defined there
 */

#define ACL_ID_PUBLIC	0		/* placeholder for id in a PUBLIC acl item */

/*
 * AclItem
 *
 * Note: must be same size on all platforms, because the size is hardcoded
 * in the pg_type.h entry for aclitem.
 */
typedef struct AclItem
{
	Oid			ai_grantee;		/* ID that this item grants privs to */
	Oid			ai_grantor;		/* grantor of privs */
	AclMode		ai_privs;		/* privilege bits */
} AclItem;

/*
 * The upper 32 bits of the ai_privs field of an AclItem are the grant option
 * bits, and the lower 32 bits are the actual privileges.  We use "rights"
 * to mean the combined grant option and privilege bits fields.
 */
#define ACLITEM_GET_PRIVS(item)    ((item).ai_privs & 0xFFFFFFFF)
#define ACLITEM_GET_GOPTIONS(item) (((item).ai_privs >> 32) & 0xFFFFFFFF)
#define ACLITEM_GET_RIGHTS(item)   ((item).ai_privs)

#define ACL_GRANT_OPTION_FOR(privs) (((AclMode) (privs) & 0xFFFFFFFF) << 32)
#define ACL_OPTION_TO_PRIVS(privs)	(((AclMode) (privs) >> 32) & 0xFFFFFFFF)

#define ACLITEM_SET_PRIVS(item,privs) \
  ((item).ai_privs = ((item).ai_privs & ~((AclMode) 0xFFFFFFFF)) | \
					 ((AclMode) (privs) & 0xFFFFFFFF))
#define ACLITEM_SET_GOPTIONS(item,goptions) \
  ((item).ai_privs = ((item).ai_privs & ~(((AclMode) 0xFFFFFFFF) << 32)) | \
					 (((AclMode) (goptions) & 0xFFFFFFFF) << 32))
#define ACLITEM_SET_RIGHTS(item,rights) \
  ((item).ai_privs = (AclMode) (rights))

#define ACLITEM_SET_PRIVS_GOPTIONS(item,privs,goptions) \
  ((item).ai_privs = ((AclMode) (privs) & 0xFFFFFFFF) | \
					 (((AclMode) (goptions) & 0xFFFFFFFF) << 32))


#define ACLITEM_ALL_PRIV_BITS		((AclMode) 0xFFFFFFFF)
#define ACLITEM_ALL_GOPTION_BITS	((AclMode) 0xFFFFFFFF << 32)

/*
 * Definitions for convenient access to Acl (array of AclItem).
 * These are standard PostgreSQL arrays, but are restricted to have one
 * dimension and no nulls.  We also ignore the lower bound when reading,
 * and set it to one when writing.
 *
 * CAUTION: as of PostgreSQL 7.1, these arrays are toastable (just like all
 * other array types).  Therefore, be careful to detoast them with the
 * macros provided, unless you know for certain that a particular array
 * can't have been toasted.
 */


/*
 * Acl			a one-dimensional array of AclItem
 */
typedef struct ArrayType Acl;

#define ACL_NUM(ACL)			(ARR_DIMS(ACL)[0])
#define ACL_DAT(ACL)			((AclItem *) ARR_DATA_PTR(ACL))
#define ACL_N_SIZE(N)			(ARR_OVERHEAD_NONULLS(1) + ((N) * sizeof(AclItem)))
#define ACL_SIZE(ACL)			ARR_SIZE(ACL)

/*
 * fmgr macros for these types
 */
#define DatumGetAclItemP(X)		   ((AclItem *) DatumGetPointer(X))
#define PG_GETARG_ACLITEM_P(n)	   DatumGetAclItemP(PG_GETARG_DATUM(n))
#define PG_RETURN_ACLITEM_P(x)	   PG_RETURN_POINTER(x)

#define DatumGetAclP(X)			   ((Acl *) PG_DETOAST_DATUM(X))
#define DatumGetAclPCopy(X)		   ((Acl *) PG_DETOAST_DATUM_COPY(X))
#define PG_GETARG_ACL_P(n)		   DatumGetAclP(PG_GETARG_DATUM(n))
#define PG_GETARG_ACL_P_COPY(n)    DatumGetAclPCopy(PG_GETARG_DATUM(n))
#define PG_RETURN_ACL_P(x)		   PG_RETURN_POINTER(x)

/*
 * ACL modification opcodes for aclupdate
 */
#define ACL_MODECHG_ADD			1
#define ACL_MODECHG_DEL			2
#define ACL_MODECHG_EQL			3

/*
 * External representations of the privilege bits --- aclitemin/aclitemout
 * represent each possible privilege bit with a distinct 1-character code
 */
#define ACL_INSERT_CHR			'a' /* formerly known as "append" */
#define ACL_SELECT_CHR			'r' /* formerly known as "read" */
#define ACL_UPDATE_CHR			'w' /* formerly known as "write" */
#define ACL_DELETE_CHR			'd'
#define ACL_TRUNCATE_CHR		'D' /* super-delete, as it were */
#define ACL_REFERENCES_CHR		'x'
#define ACL_TRIGGER_CHR			't'
#define ACL_EXECUTE_CHR			'X'
#define ACL_USAGE_CHR			'U'
#define ACL_CREATE_CHR			'C'
#define ACL_CREATE_TEMP_CHR		'T'
#define ACL_CONNECT_CHR			'c'
#define ACL_SET_CHR				's'
#define ACL_ALTER_SYSTEM_CHR	'A'

/* string holding all privilege code chars, in order by bitmask position */
#define ACL_ALL_RIGHTS_STR	"arwdDxtXUCTcsA"

/*
 * Bitmasks defining "all rights" for each supported object type
 */
#define ACL_ALL_RIGHTS_COLUMN		(ACL_INSERT|ACL_SELECT|ACL_UPDATE|ACL_REFERENCES)
#define ACL_ALL_RIGHTS_RELATION		(ACL_INSERT|ACL_SELECT|ACL_UPDATE|ACL_DELETE|ACL_TRUNCATE|ACL_REFERENCES|ACL_TRIGGER)
#define ACL_ALL_RIGHTS_SEQUENCE		(ACL_USAGE|ACL_SELECT|ACL_UPDATE)
#define ACL_ALL_RIGHTS_DATABASE		(ACL_CREATE|ACL_CREATE_TEMP|ACL_CONNECT)
#define ACL_ALL_RIGHTS_FDW			(ACL_USAGE)
#define ACL_ALL_RIGHTS_FOREIGN_SERVER (ACL_USAGE)
#define ACL_ALL_RIGHTS_FUNCTION		(ACL_EXECUTE)
#define ACL_ALL_RIGHTS_LANGUAGE		(ACL_USAGE)
#define ACL_ALL_RIGHTS_LARGEOBJECT	(ACL_SELECT|ACL_UPDATE)
#define ACL_ALL_RIGHTS_PARAMETER_ACL (ACL_SET|ACL_ALTER_SYSTEM)
#define ACL_ALL_RIGHTS_SCHEMA		(ACL_USAGE|ACL_CREATE)
#define ACL_ALL_RIGHTS_TABLESPACE	(ACL_CREATE)
#define ACL_ALL_RIGHTS_TYPE			(ACL_USAGE)

/* operation codes for pg_*_aclmask */
typedef enum
{
	ACLMASK_ALL,				/* normal case: compute all bits */
	ACLMASK_ANY					/* return when result is known nonzero */
} AclMaskHow;

/* result codes for pg_*_aclcheck */
typedef enum
{
	ACLCHECK_OK = 0,
	ACLCHECK_NO_PRIV,
	ACLCHECK_NOT_OWNER
} AclResult;


/*
 * routines used internally
 */
extern Acl *acldefault(ObjectType objtype, Oid ownerId);
extern Acl *get_user_default_acl(ObjectType objtype, Oid ownerId,
								 Oid nsp_oid);
extern void recordDependencyOnNewAcl(Oid classId, Oid objectId, int32 objsubId,
									 Oid ownerId, Acl *acl);

extern Acl *aclupdate(const Acl *old_acl, const AclItem *mod_aip,
					  int modechg, Oid ownerId, DropBehavior behavior);
extern Acl *aclnewowner(const Acl *old_acl, Oid oldOwnerId, Oid newOwnerId);
extern Acl *make_empty_acl(void);
extern Acl *aclcopy(const Acl *orig_acl);
extern Acl *aclconcat(const Acl *left_acl, const Acl *right_acl);
extern Acl *aclmerge(const Acl *left_acl, const Acl *right_acl, Oid ownerId);
extern void aclitemsort(Acl *acl);
extern bool aclequal(const Acl *left_acl, const Acl *right_acl);

extern AclMode aclmask(const Acl *acl, Oid roleid, Oid ownerId,
					   AclMode mask, AclMaskHow how);
extern int	aclmembers(const Acl *acl, Oid **roleids);

extern bool has_privs_of_role(Oid member, Oid role);
extern bool member_can_set_role(Oid member, Oid role);
extern void check_can_set_role(Oid member, Oid role);
extern bool is_member_of_role(Oid member, Oid role);
extern bool is_member_of_role_nosuper(Oid member, Oid role);
extern bool is_admin_of_role(Oid member, Oid role);
extern Oid	select_best_admin(Oid member, Oid role);
extern Oid	get_role_oid(const char *rolname, bool missing_ok);
extern Oid	get_role_oid_or_public(const char *rolname);
extern Oid	get_rolespec_oid(const RoleSpec *role, bool missing_ok);
extern void check_rolespec_name(const RoleSpec *role, const char *detail_msg);
extern HeapTuple get_rolespec_tuple(const RoleSpec *role);
extern char *get_rolespec_name(const RoleSpec *role);

extern void select_best_grantor(Oid roleId, AclMode privileges,
								const Acl *acl, Oid ownerId,
								Oid *grantorId, AclMode *grantOptions);

extern void initialize_acl(void);

/*
 * prototypes for functions in aclchk.c
 */
extern void ExecuteGrantStmt(GrantStmt *stmt);
extern void ExecAlterDefaultPrivilegesStmt(ParseState *pstate, AlterDefaultPrivilegesStmt *stmt);

extern void RemoveRoleFromObjectACL(Oid roleid, Oid classid, Oid objid);

extern AclMode pg_class_aclmask(Oid table_oid, Oid roleid,
								AclMode mask, AclMaskHow how);

/* generic function */
extern AclResult object_aclcheck(Oid classid, Oid objectid, Oid roleid, AclMode mode);

/* special cases */
extern AclResult pg_attribute_aclcheck(Oid table_oid, AttrNumber attnum,
									   Oid roleid, AclMode mode);
extern AclResult pg_attribute_aclcheck_ext(Oid table_oid, AttrNumber attnum,
										   Oid roleid, AclMode mode,
										   bool *is_missing);
extern AclResult pg_attribute_aclcheck_all(Oid table_oid, Oid roleid,
										   AclMode mode, AclMaskHow how);
extern AclResult pg_class_aclcheck(Oid table_oid, Oid roleid, AclMode mode);
extern AclResult pg_class_aclcheck_ext(Oid table_oid, Oid roleid,
									   AclMode mode, bool *is_missing);
extern AclResult pg_parameter_aclcheck(const char *name, Oid roleid,
									   AclMode mode);
extern AclResult pg_largeobject_aclcheck_snapshot(Oid lobj_oid, Oid roleid,
												  AclMode mode, Snapshot snapshot);

extern void aclcheck_error(AclResult aclerr, ObjectType objtype,
						   const char *objectname);

extern void aclcheck_error_col(AclResult aclerr, ObjectType objtype,
							   const char *objectname, const char *colname);

extern void aclcheck_error_type(AclResult aclerr, Oid typeOid);

extern void recordExtObjInitPriv(Oid objoid, Oid classoid);
extern void removeExtObjInitPriv(Oid objoid, Oid classoid);


/* ownercheck routines just return true (owner) or false (not) */
extern bool object_ownercheck(Oid classid, Oid objectid, Oid roleid);
extern bool has_createrole_privilege(Oid roleid);
extern bool has_bypassrls_privilege(Oid roleid);

#endif							/* ACL_H */
