/*-------------------------------------------------------------------------
 *
 * bootstrap.h--
 *	  include file for the bootstrapping code
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: bootstrap.h,v 1.13 1998/09/01 04:34:36 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef BOOTSTRAP_H
#define BOOTSTRAP_H

#include <access/funcindex.h>
#include <access/itup.h>
#include <utils/rel.h>

#define MAXATTR 40				/* max. number of attributes in a relation */

typedef struct hashnode
{
	int			strnum;			/* Index into string table */
	struct hashnode *next;
} hashnode;

#define EMITPROMPT printf("> ")

extern Relation reldesc;
extern Form_pg_attribute attrtypes[MAXATTR];
extern int	numattr;
extern int	DebugMode;

extern int	BootstrapMain(int ac, char *av[]);
extern void index_register(char *heap,
			   char *ind,
			   int natts,
			   AttrNumber *attnos,
			   uint16 nparams,
			   Datum *params,
			   FuncIndexInfo *finfo,
			   PredInfo *predInfo);

extern void err_out(void);
extern void InsertOneTuple(Oid objectid);
extern void closerel(char *name);
extern void boot_openrel(char *name);
extern char *LexIDStr(int ident_num);

extern void DefineAttr(char *name, char *type, int attnum);
extern void InsertOneValue(Oid objectid, char *value, int i);
extern void InsertOneNull(int i);
extern char *MapArrayTypeName(char *s);
extern char *CleanUpStr(char *s);
extern int	EnterString(char *str);
extern void build_indices(void);

#endif	 /* BOOTSTRAP_H */
