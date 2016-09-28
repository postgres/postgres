/*-------------------------------------------------------------------------
 *
 * user.h
 *	  Commands for manipulating roles (formerly called users).
 *
 *
 * src/include/commands/user.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef USER_H
#define USER_H

#include "catalog/objectaddress.h"
#include "nodes/parsenodes.h"
#include "parser/parse_node.h"


/*
 * Types of password, for Password_encryption GUC and the password_type
 * argument of the check-password hook.
 */
typedef enum PasswordType
{
	PASSWORD_TYPE_PLAINTEXT = 0,
	PASSWORD_TYPE_MD5
} PasswordType;

extern int	Password_encryption;	/* GUC */

/* Hook to check passwords in CreateRole() and AlterRole() */
typedef void (*check_password_hook_type) (const char *username, const char *password, int password_type, Datum validuntil_time, bool validuntil_null);

extern PGDLLIMPORT check_password_hook_type check_password_hook;

extern Oid	CreateRole(ParseState *pstate, CreateRoleStmt *stmt);
extern Oid	AlterRole(AlterRoleStmt *stmt);
extern Oid	AlterRoleSet(AlterRoleSetStmt *stmt);
extern void DropRole(DropRoleStmt *stmt);
extern void GrantRole(GrantRoleStmt *stmt);
extern ObjectAddress RenameRole(const char *oldname, const char *newname);
extern void DropOwnedObjects(DropOwnedStmt *stmt);
extern void ReassignOwnedObjects(ReassignOwnedStmt *stmt);
extern List *roleSpecsToIds(List *memberNames);

#endif   /* USER_H */
