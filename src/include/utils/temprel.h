/*-------------------------------------------------------------------------
 *
 * temprel.h--
 *	  Temporary relation functions
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: temprel.h,v 1.1 1999/02/02 03:45:28 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef TEMPREL_H
#define TEMPREL_H

#include "access/htup.h"
#include "access/attnum.h"

void create_temp_relation(char *relname, HeapTuple pg_class_tuple);
void remove_all_temp_relations(void);
void remove_temp_relation(Oid relid);
HeapTuple get_temp_rel_by_name(char *user_relname);

#endif	 /* TEMPREL_H */

