/*-------------------------------------------------------------------------
 *
 * user.h
 *
 *
 * $Id: user.h,v 1.12 2000/05/29 01:59:11 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef USER_H
#define USER_H

#include "nodes/parsenodes.h"
#include "access/htup.h"

extern void CreateUser(CreateUserStmt *stmt);
extern void AlterUser(AlterUserStmt *stmt);
extern void DropUser(DropUserStmt *stmt);

extern void CreateGroup(CreateGroupStmt *stmt);
extern void AlterGroup(AlterGroupStmt *stmt, const char *tag);
extern void DropGroup(DropGroupStmt *stmt);

extern Datum update_pg_pwd(PG_FUNCTION_ARGS);

#endif	 /* USER_H */
