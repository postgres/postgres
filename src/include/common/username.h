/*
 *	username.h
 *		lookup effective username
 *
 *	Copyright (c) 2003-2016, PostgreSQL Global Development Group
 *
 *	src/include/common/username.h
 */
#ifndef USERNAME_H
#define USERNAME_H

extern const char *get_user_name(char **errstr);
extern const char *get_user_name_or_exit(const char *progname);

#endif   /* USERNAME_H */
