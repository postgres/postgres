/*-------------------------------------------------------------------------
 *
 * temprel.h
 *	  Temporary relation functions
 *
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: temprel.h,v 1.9 2000/04/12 17:16:55 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef TEMPREL_H
#define TEMPREL_H

#include "access/htup.h"

void		create_temp_relation(const char *relname, HeapTuple pg_class_tuple);
void		remove_all_temp_relations(void);
void		invalidate_temp_relations(void);
void		remove_temp_relation(Oid relid);
char	   *get_temp_rel_by_username(const char *user_relname);
char	   *get_temp_rel_by_physicalname(const char *relname);

#endif	 /* TEMPREL_H */
