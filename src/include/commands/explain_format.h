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

#include "commands/explain.h"

extern void ExplainPropertyList(const char *qlabel, List *data,
								ExplainState *es);
extern void ExplainPropertyListNested(const char *qlabel, List *data,
									  ExplainState *es);
extern void ExplainPropertyText(const char *qlabel, const char *value,
								ExplainState *es);
extern void ExplainPropertyInteger(const char *qlabel, const char *unit,
								   int64 value, ExplainState *es);
extern void ExplainPropertyUInteger(const char *qlabel, const char *unit,
									uint64 value, ExplainState *es);
extern void ExplainPropertyFloat(const char *qlabel, const char *unit,
								 double value, int ndigits, ExplainState *es);
extern void ExplainPropertyBool(const char *qlabel, bool value,
								ExplainState *es);

extern void ExplainOpenGroup(const char *objtype, const char *labelname,
							 bool labeled, ExplainState *es);
extern void ExplainCloseGroup(const char *objtype, const char *labelname,
							  bool labeled, ExplainState *es);

extern void ExplainOpenSetAsideGroup(const char *objtype, const char *labelname,
									 bool labeled, int depth, ExplainState *es);
extern void ExplainSaveGroup(ExplainState *es, int depth, int *state_save);
extern void ExplainRestoreGroup(ExplainState *es, int depth, int *state_save);

extern void ExplainDummyGroup(const char *objtype, const char *labelname,
							  ExplainState *es);

extern void ExplainBeginOutput(ExplainState *es);
extern void ExplainEndOutput(ExplainState *es);
extern void ExplainSeparatePlans(ExplainState *es);

extern void ExplainIndentText(ExplainState *es);

#endif
