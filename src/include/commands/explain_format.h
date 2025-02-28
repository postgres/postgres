/*-------------------------------------------------------------------------
 *
 * explain_format.h
 *	  prototypes for explain_format.c
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994-5, Regents of the University of California
 *
 * src/include/commands/explain_format.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef EXPLAIN_FORMAT_H
#define EXPLAIN_FORMAT_H

#include "nodes/pg_list.h"

struct ExplainState;			/* avoid including explain.h here */

extern void ExplainPropertyList(const char *qlabel, List *data,
								struct ExplainState *es);
extern void ExplainPropertyListNested(const char *qlabel, List *data,
									  struct ExplainState *es);
extern void ExplainPropertyText(const char *qlabel, const char *value,
								struct ExplainState *es);
extern void ExplainPropertyInteger(const char *qlabel, const char *unit,
								   int64 value, struct ExplainState *es);
extern void ExplainPropertyUInteger(const char *qlabel, const char *unit,
									uint64 value, struct ExplainState *es);
extern void ExplainPropertyFloat(const char *qlabel, const char *unit,
								 double value, int ndigits,
								 struct ExplainState *es);
extern void ExplainPropertyBool(const char *qlabel, bool value,
								struct ExplainState *es);

extern void ExplainOpenGroup(const char *objtype, const char *labelname,
							 bool labeled, struct ExplainState *es);
extern void ExplainCloseGroup(const char *objtype, const char *labelname,
							  bool labeled, struct ExplainState *es);

extern void ExplainOpenSetAsideGroup(const char *objtype, const char *labelname,
									 bool labeled, int depth,
									 struct ExplainState *es);
extern void ExplainSaveGroup(struct ExplainState *es, int depth,
							 int *state_save);
extern void ExplainRestoreGroup(struct ExplainState *es, int depth,
								int *state_save);

extern void ExplainDummyGroup(const char *objtype, const char *labelname,
							  struct ExplainState *es);

extern void ExplainBeginOutput(struct ExplainState *es);
extern void ExplainEndOutput(struct ExplainState *es);
extern void ExplainSeparatePlans(struct ExplainState *es);

extern void ExplainIndentText(struct ExplainState *es);

#endif
