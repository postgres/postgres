/*-------------------------------------------------------------------------
 *
 * user.h
 *	  Commands for manipulating users and groups.
 *
 *
 * $Id: user.h,v 1.20 2002/10/21 19:46:45 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef USER_H
#define USER_H

#include "fmgr.h"
#include "nodes/parsenodes.h"


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

extern void AtEOXact_UpdatePasswordFile(bool isCommit);

#endif   /* USER_H */
