/*-------------------------------------------------------------------------
 *
 * bootstrap.h
 *	  include file for the bootstrapping code
 *
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: bootstrap.h,v 1.19 2000/07/14 22:17:54 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef BOOTSTRAP_H
#define BOOTSTRAP_H

#include "access/itup.h"
#include "nodes/execnodes.h"
#include "utils/rel.h"

/* MAXATTR is the maximum number of attributes in a relation supported
 * at bootstrap time (ie, the max possible in a system table).
 */
#define MAXATTR 40

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

extern void index_register(char *heap, char *ind, IndexInfo *indexInfo);

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

extern int	Int_yylex(void);
extern void Int_yyerror(const char *str);

#endif	 /* BOOTSTRAP_H */
