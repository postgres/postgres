/*
 *	connstrings.h
 *		connecting string processing prototypes
 *
 *	Copyright (c) 2012-2015, PostgreSQL Global Development Group
 *
 *	src/include/common/connstrings.h
 */
#ifndef CONNSTRINGS_H
#define CONNSTRINGS_H


extern int	libpq_connstring_uri_prefix_length(const char *connstr);
extern bool libpq_connstring_is_recognized(const char *connstr);

#endif   /* CONNSTRINGS_H */
