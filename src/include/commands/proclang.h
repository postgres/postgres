/*-------------------------------------------------------------------------
 *
 * proclang.h
 *	  prototypes for proclang.c.
 *
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROCLANG_H
#define PROCLANG_H

#include "nodes/parsenodes.h"

extern void CreateProceduralLanguage(CreatePLangStmt *stmt);
extern void DropProceduralLanguage(DropPLangStmt *stmt);
extern void DropProceduralLanguageById(Oid langOid);

#endif   /* PROCLANG_H */
