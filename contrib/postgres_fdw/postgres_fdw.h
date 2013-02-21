/*-------------------------------------------------------------------------
 *
 * postgres_fdw.h
 *		  Foreign-data wrapper for remote PostgreSQL servers
 *
 * Portions Copyright (c) 2012-2013, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		  contrib/postgres_fdw/postgres_fdw.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef POSTGRES_FDW_H
#define POSTGRES_FDW_H

#include "foreign/foreign.h"
#include "lib/stringinfo.h"
#include "nodes/relation.h"
#include "utils/rel.h"

#include "libpq-fe.h"

/* in connection.c */
extern PGconn *GetConnection(ForeignServer *server, UserMapping *user);
extern void ReleaseConnection(PGconn *conn);
extern unsigned int GetCursorNumber(PGconn *conn);
extern void pgfdw_report_error(int elevel, PGresult *res, bool clear,
				   const char *sql);

/* in option.c */
extern int ExtractConnectionOptions(List *defelems,
						 const char **keywords,
						 const char **values);

/* in deparse.c */
extern void classifyConditions(PlannerInfo *root,
				   RelOptInfo *baserel,
				   List **remote_conds,
				   List **param_conds,
				   List **local_conds,
				   List **param_numbers);
extern void deparseSimpleSql(StringInfo buf,
				 PlannerInfo *root,
				 RelOptInfo *baserel,
				 List *local_conds);
extern void appendWhereClause(StringInfo buf,
				  bool has_where,
				  List *exprs,
				  PlannerInfo *root);
extern void deparseAnalyzeSql(StringInfo buf, Relation rel);

#endif   /* POSTGRES_FDW_H */
