/*-------------------------------------------------------------------------
 *
 * temprel.h
 *	  Temporary relation functions
 *
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: temprel.h,v 1.12 2000/11/08 22:10:03 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef TEMPREL_H
#define TEMPREL_H

#include "access/htup.h"

extern void create_temp_relation(const char *relname,
								 HeapTuple pg_class_tuple);
extern void remove_temp_rel_by_relid(Oid relid);
extern bool rename_temp_relation(const char *oldname,
								 const char *newname);

extern void remove_all_temp_relations(void);
extern void AtEOXact_temp_relations(bool isCommit);

extern char *get_temp_rel_by_username(const char *user_relname);
extern char *get_temp_rel_by_physicalname(const char *relname);

#endif	 /* TEMPREL_H */
