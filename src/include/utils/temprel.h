/*-------------------------------------------------------------------------
 *
 * temprel.h
 *	  Temporary relation functions
 *
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: temprel.h,v 1.10 2000/06/20 06:41:11 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef TEMPREL_H
#define TEMPREL_H

#include "access/htup.h"

extern void create_temp_relation(const char *relname,
								 HeapTuple pg_class_tuple);
extern void remove_temp_relation(Oid relid);
extern bool rename_temp_relation(const char *oldname,
								 const char *newname);

extern void remove_all_temp_relations(void);
extern void invalidate_temp_relations(void);

extern char *get_temp_rel_by_username(const char *user_relname);
extern char *get_temp_rel_by_physicalname(const char *relname);

#endif	 /* TEMPREL_H */
