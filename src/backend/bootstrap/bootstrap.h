/*-------------------------------------------------------------------------
 *
 * bootstrap.h--
 *    include file for the bootstrapping code
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: bootstrap.h,v 1.1.1.1 1996/07/09 06:21:14 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef BOOTSTRAP_H
#define BOOTSTRAP_H

#include <sys/file.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <ctype.h>

#include "access/htup.h"
#include "access/itup.h"
#include "access/relscan.h"
#include "access/skey.h"
#include "utils/tqual.h"
#include "storage/buf.h"
#include "storage/bufmgr.h"	/* for BufferManagerFlush */
#include "utils/portal.h"
#include "utils/elog.h"
#include "utils/rel.h"

#define	MAXATTR 40		/* max. number of attributes in a relation */

typedef struct hashnode {
    int		strnum;		/* Index into string table */
    struct hashnode	*next;
} hashnode;

#define EMITPROMPT printf("> ")

extern Relation reldesc;
extern AttributeTupleForm attrtypes[MAXATTR];
extern int numattr;
extern int DebugMode;

extern int BootstrapMain(int ac, char *av[]);
extern void index_register(char *heap,
			   char *ind,
			   int natts,
			   AttrNumber *attnos,
			   uint16 nparams,
			   Datum *params,
			   FuncIndexInfo *finfo,
			   PredInfo *predInfo);

extern void err(void);
extern void InsertOneTuple(Oid objectid);
extern void closerel(char *name);
extern void boot_openrel(char *name);
extern char *LexIDStr(int ident_num);

extern void DefineAttr(char *name, char *type, int attnum);
extern void InsertOneValue(Oid objectid, char *value, int i);
extern void InsertOneNull(int i);
extern bool BootstrapAlreadySeen(Oid id);
extern void cleanup(void);
extern int gettype(char *type);
extern AttributeTupleForm AllocateAttribute(void);
extern char* MapArrayTypeName(char *s);
extern char* CleanUpStr(char *s);
extern int EnterString (char *str);
extern int CompHash (char *str, int len);
extern hashnode *FindStr (char *str, int length, hashnode *mderef);
extern hashnode *AddStr(char *str, int strlength, int mderef);
extern void build_indices(void);

#endif /* BOOTSTRAP_H */
