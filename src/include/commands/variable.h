/*
 * Headers for handling of 'SET var TO', 'SHOW var' and 'RESET var'
 * statements
 *
 * $Id: variable.h,v 1.10 2000/05/31 00:28:37 petere Exp $
 *
 */
#ifndef VARIABLE_H
#define VARIABLE_H

extern void SetPGVariable(const char *name, const char *value);
extern void GetPGVariable(const char *name);
extern void ResetPGVariable(const char *name);

extern void set_default_datestyle(void);

#endif	 /* VARIABLE_H */
