/*-------------------------------------------------------------------------
 *
 * user.h
 *	  Commands for manipulating users and groups.
 *
 *
 * $PostgreSQL: pgsql/src/include/commands/user.h,v 1.23 2004/07/28 14:23:31 tgl Exp $
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
extern void RenameUser(const char *oldname, const char *newname);

extern void CreateGroup(CreateGroupStmt *stmt);
extern void AlterGroup(AlterGroupStmt *stmt, const char *tag);
extern void DropGroup(DropGroupStmt *stmt);
extern void RenameGroup(const char *oldname, const char *newname);

extern Datum update_pg_pwd_and_pg_group(PG_FUNCTION_ARGS);

extern void AtEOXact_UpdatePasswordFile(bool isCommit);
extern void AtEOSubXact_UpdatePasswordFile(bool isCommit, TransactionId myXid,
										   TransactionId parentXid);

#endif   /* USER_H */
