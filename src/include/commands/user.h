/*-------------------------------------------------------------------------
 *
 * user.h
 *
 *
 * $Id: user.h,v 1.16 2001/11/05 17:46:33 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef USER_H
#define USER_H

#include "nodes/parsenodes.h"

extern void CreateUser(CreateUserStmt *stmt);
extern void AlterUser(AlterUserStmt *stmt);
extern void DropUser(DropUserStmt *stmt);

extern void CreateGroup(CreateGroupStmt *stmt);
extern void AlterGroup(AlterGroupStmt *stmt, const char *tag);
extern void DropGroup(DropGroupStmt *stmt);

extern Datum update_pg_pwd(PG_FUNCTION_ARGS);

#endif   /* USER_H */
