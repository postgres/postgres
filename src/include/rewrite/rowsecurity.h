/* -------------------------------------------------------------------------
 *
 * rowsecurity.h
 *
 *    prototypes for rewrite/rowsecurity.c and the structures for managing
 *    the row security policies for relations in relcache.
 *
 * Portions Copyright (c) 1996-2015, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * -------------------------------------------------------------------------
 */
#ifndef ROWSECURITY_H
#define ROWSECURITY_H

#include "nodes/parsenodes.h"
#include "utils/array.h"
#include "utils/relcache.h"

typedef struct RowSecurityPolicy
{
	Oid					policy_id;		/* OID of the policy */
	char			   *policy_name;	/* Name of the policy */
	char				polcmd;			/* Type of command policy is for */
	ArrayType		   *roles;			/* Array of roles policy is for */
	Expr			   *qual;			/* Expression to filter rows */
	Expr			   *with_check_qual; /* Expression to limit rows allowed */
	bool				hassublinks;	/* If either expression has sublinks */
} RowSecurityPolicy;

typedef struct RowSecurityDesc
{
	MemoryContext		rscxt;		/* row security memory context */
	List			   *policies;	/* list of row security policies */
} RowSecurityDesc;

/* GUC variable */
extern int row_security;

/* Possible values for row_security GUC */
typedef enum RowSecurityConfigType
{
	ROW_SECURITY_OFF,		/* RLS never applied- error thrown if no priv */
	ROW_SECURITY_ON,		/* normal case, RLS applied for regular users */
	ROW_SECURITY_FORCE		/* RLS applied for superusers and table owners */
} RowSecurityConfigType;

/*
 * Used by callers of check_enable_rls.
 *
 * RLS could be completely disabled on the tables involved in the query,
 * which is the simple case, or it may depend on the current environment
 * (the role which is running the query or the value of the row_security
 * GUC- on, off, or force), or it might be simply enabled as usual.
 *
 * If RLS isn't on the table involved then RLS_NONE is returned to indicate
 * that we don't need to worry about invalidating the query plan for RLS
 * reasons.  If RLS is on the table, but we are bypassing it for now, then
 * we return RLS_NONE_ENV to indicate that, if the environment changes,
 * we need to invalidate and replan.  Finally, if RLS should be turned on
 * for the query, then we return RLS_ENABLED, which means we also need to
 * invalidate if the environment changes.
 */
enum CheckEnableRlsResult
{
	RLS_NONE,
	RLS_NONE_ENV,
	RLS_ENABLED
};

typedef List *(*row_security_policy_hook_type)(CmdType cmdtype,
											   Relation relation);

extern PGDLLIMPORT row_security_policy_hook_type row_security_policy_hook;

extern bool prepend_row_security_policies(Query* root, RangeTblEntry* rte,
									   int rt_index);

extern int check_enable_rls(Oid relid, Oid checkAsUser);

#endif	/* ROWSECURITY_H */
