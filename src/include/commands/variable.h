/*
 * Headers for handling of 'SET var TO', 'SHOW var' and 'RESET var'
 * statements
 *
 * $Id: variable.h,v 1.1 1998/01/05 18:53:08 momjian Exp $
 *
 */
#ifndef VARIABLE_H
#define VARIABLE_H 1

enum DateFormat
{
	Date_Postgres, Date_SQL, Date_ISO
};

/*-----------------------------------------------------------------------*/
struct PGVariables
{
	struct
	{
		bool		euro;
		enum DateFormat format;
	}			date;
};

extern struct PGVariables PGVariables;

/*-----------------------------------------------------------------------*/
bool		SetPGVariable(const char *, const char *);
bool		GetPGVariable(const char *);
bool		ResetPGVariable(const char *);

extern bool set_date(void);
extern bool show_date(void);
extern bool reset_date(void);
extern bool parse_date(const char *);
extern bool set_timezone(void);
extern bool show_timezone(void);
extern bool reset_timezone(void);
extern bool parse_timezone(const char *);
extern bool set_cost_heap(void);
extern bool show_cost_heap(void);
extern bool reset_cost_heap(void);
extern bool parse_cost_heap(const char *);
extern bool set_cost_index(void);
extern bool show_cost_index(void);
extern bool reset_cost_index(void);
extern bool parse_cost_index(const char *);
extern bool set_r_plans(void);
extern bool show_r_plans(void);
extern bool reset_r_plans(void);
extern bool parse_r_plans(const char *);
extern bool set_geqo(void);
extern bool show_geqo(void);
extern bool reset_geqo(void);
extern bool parse_geqo(const char *);

#endif /* VARIABLE_H */
