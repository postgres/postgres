/*-------------------------------------------------------------------------
 *
 * user.h
 *
 *
 * $Id: user.h,v 1.18 2002/04/04 04:25:53 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef USER_H
#define USER_H

#include "fmgr.h"
#include "nodes/parsenodes.h"

#define PWD_FILE	"pg_pwd"

#define USER_GROUP_FILE	"pg_group"


extern char *group_getfilename(void);
extern char *user_getfilename(void);
extern void CreateUser(CreateUserStmt *stmt);
extern void AlterUser(AlterUserStmt *stmt);
extern void AlterUserSet(AlterUserSetStmt *stmt);
extern void DropUser(DropUserStmt *stmt);

extern void CreateGroup(CreateGroupStmt *stmt);
extern void AlterGroup(AlterGroupStmt *stmt, const char *tag);
extern void DropGroup(DropGroupStmt *stmt);

extern Datum update_pg_pwd_and_pg_group(PG_FUNCTION_ARGS);

#endif   /* USER_H */
