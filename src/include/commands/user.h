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

extern void DefineUser(CreateUserStmt *stmt, CommandDest);
extern void AlterUser(AlterUserStmt *stmt, CommandDest);
extern void RemoveUser(char *user, CommandDest);

#endif	 /* USER_H */
