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
extern void RenameLanguage(const char *oldname, const char *newname);
extern void AlterLanguageOwner(const char *name, Oid newOwnerId);
extern void AlterLanguageOwner_oid(Oid oid, Oid newOwnerId);
extern bool PLTemplateExists(const char *languageName);

#endif   /* PROCLANG_H */
