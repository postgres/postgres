/*
 * Headers for handling of 'SET var TO', 'SHOW var' and 'RESET var'
 * statements
 *
 * $Id: variable.h,v 1.11 2000/06/09 01:44:24 momjian Exp $
 *
 */
#ifndef VARIABLE_H
#define VARIABLE_H

extern void SetPGVariable(const char *name, const char *value);
extern void GetPGVariable(const char *name);
extern void ResetPGVariable(const char *name);

extern void set_default_datestyle(void);
extern bool examine_subclass;

#endif	 /* VARIABLE_H */
