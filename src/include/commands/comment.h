/*
 * src/include/commands/comment.h
 *
 *-------------------------------------------------------------------------
 *
 * comment.h
 *
 * Prototypes for functions in commands/comment.c
 *
 * Copyright (c) 1999-2016, PostgreSQL Global Development Group
 *
 *-------------------------------------------------------------------------
 */

#ifndef COMMENT_H
#define COMMENT_H

#include "catalog/objectaddress.h"
#include "nodes/parsenodes.h"

/*------------------------------------------------------------------
 * Function Prototypes --
 *
 * The following prototypes define the public functions of the comment
 * related routines.  CommentObject() implements the SQL "COMMENT ON"
 * command.  DeleteComments() deletes all comments for an object.
 * CreateComments creates (or deletes, if comment is NULL) a comment
 * for a specific key.  There are versions of these two methods for
 * both normal and shared objects.
 *------------------------------------------------------------------
 */

extern ObjectAddress CommentObject(CommentStmt *stmt);

extern void DeleteComments(Oid oid, Oid classoid, int32 subid);

extern void CreateComments(Oid oid, Oid classoid, int32 subid, char *comment);

extern void DeleteSharedComments(Oid oid, Oid classoid);

extern void CreateSharedComments(Oid oid, Oid classoid, char *comment);

extern char *GetComment(Oid oid, Oid classoid, int32 subid);

#endif   /* COMMENT_H */
