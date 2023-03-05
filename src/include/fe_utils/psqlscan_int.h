/*-------------------------------------------------------------------------
 *
 * psqlscan_int.h
 *	  lexical scanner internal declarations
 *
 * This file declares the PsqlScanStateData structure used by psqlscan.l
 * and shared by other lexers compatible with it, such as psqlscanslash.l.
 *
 * One difficult aspect of this code is that we need to work in multibyte
 * encodings that are not ASCII-safe.  A "safe" encoding is one in which each
 * byte of a multibyte character has the high bit set (it's >= 0x80).  Since
 * all our lexing rules treat all high-bit-set characters alike, we don't
 * really need to care whether such a byte is part of a sequence or not.
 * In an "unsafe" encoding, we still expect the first byte of a multibyte
 * sequence to be >= 0x80, but later bytes might not be.  If we scan such
 * a sequence as-is, the lexing rules could easily be fooled into matching
 * such bytes to ordinary ASCII characters.  Our solution for this is to
 * substitute 0xFF for each non-first byte within the data presented to flex.
 * The flex rules will then pass the FF's through unmolested.  The
 * psqlscan_emit() subroutine is responsible for looking back to the original
 * string and replacing FF's with the corresponding original bytes.
 *
 * Another interesting thing we do here is scan different parts of the same
 * input with physically separate flex lexers (ie, lexers written in separate
 * .l files).  We can get away with this because the only part of the
 * persistent state of a flex lexer that depends on its parsing rule tables
 * is the start state number, which is easy enough to manage --- usually,
 * in fact, we just need to set it to INITIAL when changing lexers.  But to
 * make that work at all, we must use re-entrant lexers, so that all the
 * relevant state is in the yyscan_t attached to the PsqlScanState;
 * if we were using lexers with separate static state we would soon end up
 * with dangling buffer pointers in one or the other.  Also note that this
 * is unlikely to work very nicely if the lexers aren't all built with the
 * same flex version, or if they don't use the same flex options.
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/fe_utils/psqlscan_int.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PSQLSCAN_INT_H
#define PSQLSCAN_INT_H

#include "fe_utils/psqlscan.h"

/*
 * These are just to allow this file to be compilable standalone for header
 * validity checking; in actual use, this file should always be included
 * from the body of a flex file, where these symbols are already defined.
 */
#ifndef YY_TYPEDEF_YY_BUFFER_STATE
#define YY_TYPEDEF_YY_BUFFER_STATE
typedef struct yy_buffer_state *YY_BUFFER_STATE;
#endif
#ifndef YY_TYPEDEF_YY_SCANNER_T
#define YY_TYPEDEF_YY_SCANNER_T
typedef void *yyscan_t;
#endif

/*
 * We use a stack of flex buffers to handle substitution of psql variables.
 * Each stacked buffer contains the as-yet-unread text from one psql variable.
 * When we pop the stack all the way, we resume reading from the outer buffer
 * identified by scanbufhandle.
 */
typedef struct StackElem
{
	YY_BUFFER_STATE buf;		/* flex input control structure */
	char	   *bufstring;		/* data actually being scanned by flex */
	char	   *origstring;		/* copy of original data, if needed */
	char	   *varname;		/* name of variable providing data, or NULL */
	struct StackElem *next;
} StackElem;

/*
 * All working state of the lexer must be stored in PsqlScanStateData
 * between calls.  This allows us to have multiple open lexer operations,
 * which is needed for nested include files.  The lexer itself is not
 * recursive, but it must be re-entrant.
 */
typedef struct PsqlScanStateData
{
	yyscan_t	scanner;		/* Flex's state for this PsqlScanState */

	PQExpBuffer output_buf;		/* current output buffer */

	StackElem  *buffer_stack;	/* stack of variable expansion buffers */

	/*
	 * These variables always refer to the outer buffer, never to any stacked
	 * variable-expansion buffer.
	 */
	YY_BUFFER_STATE scanbufhandle;
	char	   *scanbuf;		/* start of outer-level input buffer */
	const char *scanline;		/* current input line at outer level */

	/* safe_encoding, curline, refline are used by emit() to replace FFs */
	int			encoding;		/* encoding being used now */
	bool		safe_encoding;	/* is current encoding "safe"? */
	bool		std_strings;	/* are string literals standard? */
	const char *curline;		/* actual flex input string for cur buf */
	const char *refline;		/* original data for cur buffer */

	/*
	 * All this state lives across successive input lines, until explicitly
	 * reset by psql_scan_reset.  start_state is adopted by yylex() on entry,
	 * and updated with its finishing state on exit.
	 */
	int			start_state;	/* yylex's starting/finishing state */
	int			state_before_str_stop;	/* start cond. before end quote */
	int			paren_depth;	/* depth of nesting in parentheses */
	int			xcdepth;		/* depth of nesting in slash-star comments */
	char	   *dolqstart;		/* current $foo$ quote start string */

	/*
	 * State to track boundaries of BEGIN ... END blocks in function
	 * definitions, so that semicolons do not send query too early.
	 */
	int			identifier_count;	/* identifiers since start of statement */
	char		identifiers[4]; /* records the first few identifiers */
	int			begin_depth;	/* depth of begin/end pairs */

	/*
	 * Callback functions provided by the program making use of the lexer,
	 * plus a void* callback passthrough argument.
	 */
	const PsqlScanCallbacks *callbacks;
	void	   *cb_passthrough;
} PsqlScanStateData;


/*
 * Functions exported by psqlscan.l, but only meant for use within
 * compatible lexers.
 */
extern void psqlscan_push_new_buffer(PsqlScanState state,
									 const char *newstr, const char *varname);
extern void psqlscan_pop_buffer_stack(PsqlScanState state);
extern void psqlscan_select_top_buffer(PsqlScanState state);
extern bool psqlscan_var_is_current_source(PsqlScanState state,
										   const char *varname);
extern YY_BUFFER_STATE psqlscan_prepare_buffer(PsqlScanState state,
											   const char *txt, int len,
											   char **txtcopy);
extern void psqlscan_emit(PsqlScanState state, const char *txt, int len);
extern char *psqlscan_extract_substring(PsqlScanState state,
										const char *txt, int len);
extern void psqlscan_escape_variable(PsqlScanState state,
									 const char *txt, int len,
									 PsqlScanQuoteType quote);
extern void psqlscan_test_variable(PsqlScanState state,
								   const char *txt, int len);

#endif							/* PSQLSCAN_INT_H */
