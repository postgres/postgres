/*-------------------------------------------------------------------------
 *
 * user.h
 *
 *
 *
 *
 *-------------------------------------------------------------------------
 */
#ifndef USER_H
#define USER_H

#include "nodes/parsenodes.h"
#include "tcop/dest.h"

extern void DefineUser(CreateUserStmt *stmt, CommandDest);
extern void AlterUser(AlterUserStmt *stmt, CommandDest);
extern void RemoveUser(char *user, CommandDest);

extern void CreateGroup(CreateGroupStmt *stmt, CommandDest dest);
extern void AlterGroup(AlterGroupStmt *stmt, CommandDest dest);
extern void DropGroup(DropGroupStmt *stmt, CommandDest dest);

extern HeapTuple update_pg_pwd(void);

#endif	 /* USER_H */
