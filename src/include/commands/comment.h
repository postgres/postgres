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

/*------------------------------------------------------------------
 * Function Prototypes --
 *
 * The following protoypes define the public functions of the comment
 * related routines. CreateComments() is used to create/drop a comment
 * for any object with a valid oid. DeleteComments() deletes, if any,
 * the comments associated with the object. CommentObject() is used to
 * create comments to be identified by the specific type.
 *------------------------------------------------------------------
 */

void		CreateComments(Oid oid, char *comment);
void		DeleteComments(Oid oid);
void CommentObject(int objtype, char *objname, char *objproperty,
			  List *objlist, char *comment);

#endif	 /* COMMENT_H */
