/*-------------------------------------------------------------------------
 *
 * readfuncs.h
 *	  header file for read.c and readfuncs.c. These functions are internal
 *	  to the stringToNode interface and should not be used by anyone else.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/nodes/readfuncs.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef READFUNCS_H
#define READFUNCS_H

#include "nodes/nodes.h"

/*
 * ReadNodeContext - Context data for node deserialization
 *
 * This struct holds the state and some configuration of the deserializer.
 */
typedef struct ReadNodeContext
{
	/* the string that's being parsed */
	const char *str;
#ifdef DEBUG_NODE_TESTS_ENABLED
	/* state flag determining how readfuncs.c should treat location fields */
	bool		restore_location_fields;
#endif
} ReadNodeContext;

/*
 * prototypes for functions in read.c (the lisp token parser)
 */
extern const char *pg_strtok(ReadNodeContext *ctx, int *length);
extern char *debackslash(const char *token, int length);
extern void *nodeRead(ReadNodeContext *ctx, const char *token, int tok_len);

/*
 * prototypes for functions in readfuncs.c
 */
extern Node *parseNodeString(ReadNodeContext *ctx);
extern struct Bitmapset *readBitmapset(ReadNodeContext *ctx);
extern Datum readDatum(ReadNodeContext *ctx, bool typbyval);
extern bool *readBoolCols(ReadNodeContext *ctx, int numCols);
extern int *readIntCols(ReadNodeContext *ctx, int numCols);
extern Oid *readOidCols(ReadNodeContext *ctx, int numCols);
extern int16 *readAttrNumberCols(ReadNodeContext *ctx, int numCols);

#endif							/* READFUNCS_H */
