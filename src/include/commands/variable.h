/*
 * Headers for handling of 'SET var TO', 'SHOW var' and 'RESET var'
 * statements
 *
 * $Id: variable.h,v 1.17 2001/11/05 17:46:33 momjian Exp $
 *
 */
#ifndef VARIABLE_H
#define VARIABLE_H

extern void SetPGVariable(const char *name, List *args);
extern void GetPGVariable(const char *name);
extern void ResetPGVariable(const char *name);

extern void set_default_datestyle(void);
extern void set_default_client_encoding(void);

#endif   /* VARIABLE_H */
