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

extern void DefineUser(CreateUserStmt *stmt);
extern void AlterUser(AlterUserStmt *stmt);
extern void RemoveUser(char *user);

#endif	 /* USER_H */
