/*-------------------------------------------------------------------------
 *
 * comment.h
 *
 * Prototypes for functions in commands/comment.c
 *
 * Copyright (c) 1999, PostgreSQL Global Development Group
 *
 *-------------------------------------------------------------------------
 */

#ifndef COMMENT_H
#define COMMENT_H

#include "nodes/pg_list.h"

/*------------------------------------------------------------------
 * Function Prototypes --
 *
 * The following protoypes define the public functions of the comment
 * related routines.  CommentObject() implements the SQL "COMMENT ON"
 * command.  DeleteComments() deletes all comments for an object.
 * CreateComments creates (or deletes, if comment is NULL) a comment
 * for a specific key.
 *------------------------------------------------------------------
 */

extern void CommentObject(int objtype, char *objname, char *objproperty,
			  List *objlist, char *comment);

extern void DeleteComments(Oid oid, Oid classoid);

extern void CreateComments(Oid oid, Oid classoid, int32 subid, char *comment);

#endif   /* COMMENT_H */
