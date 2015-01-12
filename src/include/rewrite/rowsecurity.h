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

typedef List *(*row_security_policy_hook_type)(CmdType cmdtype,
											   Relation relation);

extern PGDLLIMPORT row_security_policy_hook_type row_security_policy_hook;

extern bool prepend_row_security_policies(Query* root, RangeTblEntry* rte,
									   int rt_index);

#endif	/* ROWSECURITY_H */
