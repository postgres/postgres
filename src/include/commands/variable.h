/*
 * Headers for handling of 'SET var TO', 'SHOW var' and 'RESET var'
 * statements
 *
 * $Id: variable.h,v 1.16 2001/10/28 06:26:06 momjian Exp $
 *
 */
#ifndef VARIABLE_H
#define VARIABLE_H

extern void SetPGVariable(const char *name, List *args);
extern void GetPGVariable(const char *name);
extern void ResetPGVariable(const char *name);

extern void set_default_datestyle(void);
extern void set_default_client_encoding(void);

#endif	 /* VARIABLE_H */
