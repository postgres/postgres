/*-------------------------------------------------------------------------
 *
 * regexport.h
 *	  Declarations for exporting info about a regex's NFA (nondeterministic
 *	  finite automaton)
 *
 * The functions declared here provide accessors to extract the NFA state
 * graph and color character sets of a successfully-compiled regex.
 *
 * An NFA contains one or more states, numbered 0..N-1.  There is an initial
 * state, as well as a final state --- reaching the final state denotes
 * successful matching of an input string.  Each state except the final one
 * has some out-arcs that lead to successor states, each arc being labeled
 * with a color that represents one or more concrete character codes.
 * (The colors of a state's out-arcs need not be distinct, since this is an
 * NFA not a DFA.)	There are also "pseudocolors" representing start/end of
 * line and start/end of string.  Colors are numbered 0..C-1, but note that
 * color 0 is "white" (all unused characters) and can generally be ignored.
 *
 * Portions Copyright (c) 2013-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1998, 1999 Henry Spencer
 *
 * IDENTIFICATION
 *	  src/include/regex/regexport.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef _REGEXPORT_H_
#define _REGEXPORT_H_

#include "regex/regex.h"

/* These macros must match corresponding ones in regguts.h: */
#define COLOR_WHITE		0		/* color for chars not appearing in regex */
#define COLOR_RAINBOW	(-2)	/* represents all colors except pseudocolors */

/* information about one arc of a regex's NFA */
typedef struct
{
	int			co;				/* label (character-set color) of arc */
	int			to;				/* next state number */
} regex_arc_t;


/* Functions for gathering information about NFA states and arcs */
extern int	pg_reg_getnumstates(const regex_t *regex);
extern int	pg_reg_getinitialstate(const regex_t *regex);
extern int	pg_reg_getfinalstate(const regex_t *regex);
extern int	pg_reg_getnumoutarcs(const regex_t *regex, int st);
extern void pg_reg_getoutarcs(const regex_t *regex, int st,
							  regex_arc_t *arcs, int arcs_len);

/* Functions for gathering information about colors */
extern int	pg_reg_getnumcolors(const regex_t *regex);
extern int	pg_reg_colorisbegin(const regex_t *regex, int co);
extern int	pg_reg_colorisend(const regex_t *regex, int co);
extern int	pg_reg_getnumcharacters(const regex_t *regex, int co);
extern void pg_reg_getcharacters(const regex_t *regex, int co,
								 pg_wchar *chars, int chars_len);

#endif							/* _REGEXPORT_H_ */
