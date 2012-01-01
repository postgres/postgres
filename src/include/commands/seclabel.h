/*
 * seclabel.h
 *
 * Prototypes for functions in commands/seclabel.c
 *
 * Portions Copyright (c) 1996-2012, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 */
#ifndef SECLABEL_H
#define SECLABEL_H

#include "catalog/objectaddress.h"

/*
 * Internal APIs
 */
extern char *GetSecurityLabel(const ObjectAddress *object,
				 const char *provider);
extern void SetSecurityLabel(const ObjectAddress *object,
				 const char *provider, const char *label);
extern void DeleteSecurityLabel(const ObjectAddress *object);
extern void DeleteSharedSecurityLabel(Oid objectId, Oid classId);

/*
 * Statement and ESP hook support
 */
extern void ExecSecLabelStmt(SecLabelStmt *stmt);

typedef void (*check_object_relabel_type) (const ObjectAddress *object,
													   const char *seclabel);
extern void register_label_provider(const char *provider,
						check_object_relabel_type hook);

#endif   /* SECLABEL_H */
