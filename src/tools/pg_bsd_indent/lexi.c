/*-
 * Copyright (c) 1985 Sun Microsystems, Inc.
 * Copyright (c) 1980, 1993
 *	The Regents of the University of California.  All rights reserved.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#if 0
#ifndef lint
static char sccsid[] = "@(#)lexi.c	8.1 (Berkeley) 6/6/93";
#endif /* not lint */
#endif

#include "c.h"

/*
 * Here we have the token scanner for indent.  It scans off one token and puts
 * it in the global variable "token".  It returns a code, indicating the type
 * of token scanned.
 */

#include <err.h>
#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include "indent_globs.h"
#include "indent_codes.h"
#include "indent.h"

#define alphanum 1
#ifdef undef
#define opchar 3
#endif

struct templ {
    const char *rwd;
    int         rwcode;
};

/*
 * This table has to be sorted alphabetically, because it'll be used in binary
 * search. For the same reason, string must be the first thing in struct templ.
 */
struct templ specials[] =
{
    {"_Bool", 4},
    {"_Complex", 4},
    {"_Imaginary", 4},
    {"auto", 10},
    {"bool", 4},
    {"break", 9},
    {"case", 8},
    {"char", 4},
    {"complex", 4},
    {"const", 4},
    {"continue", 12},
    {"default", 8},
    {"do", 6},
    {"double", 4},
    {"else", 6},
    {"enum", 3},
    {"extern", 10},
    {"float", 4},
    {"for", 5},
    {"global", 4},
    {"goto", 9},
    {"if", 5},
    {"imaginary", 4},
    {"inline", 12},
    {"int", 4},
    {"long", 4},
    {"offsetof", 1},
    {"register", 10},
    {"restrict", 12},
    {"return", 9},
    {"short", 4},
    {"signed", 4},
    {"sizeof", 2},
    {"static", 10},
    {"struct", 3},
    {"switch", 7},
    {"typedef", 11},
    {"union", 3},
    {"unsigned", 4},
    {"void", 4},
    {"volatile", 4},
    {"while", 5}
};

const char **typenames;
int         typename_count;
int         typename_top = -1;

char        chartype[128] =
{				/* this is used to facilitate the decision of
				 * what type (alphanumeric, operator) each
				 * character is */
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 3, 0, 0, 1, 3, 3, 0,
    0, 0, 3, 3, 0, 3, 0, 3,
    1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 0, 0, 3, 3, 3, 3,
    0, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 0, 0, 0, 3, 1,
    0, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 0, 3, 0, 3, 0
};

static int
strcmp_type(const void *e1, const void *e2)
{
    return (strcmp(e1, *(const char * const *)e2));
}

/*
 * Decide whether "foo(..." is a function definition or declaration.
 *
 * At call, we are looking at the '('.  Look ahead to find the first
 * '{', ';' or ',' that is not within parentheses or comments; then
 * it's a definition if we found '{', otherwise a declaration.
 * Note that this rule is fooled by K&R-style parameter declarations,
 * but telling the difference between those and function attributes
 * seems like more trouble than it's worth.  This code could also be
 * fooled by mismatched parens or apparent comment starts within string
 * literals, but that seems unlikely in the context it's used in.
 */
static int
is_func_definition(char *tp)
{
    int		paren_depth = 0;
    int		in_comment = false;
    int		in_slash_comment = false;
    int		lastc = 0;

    /* We may need to look past the end of the current buffer. */
    lookahead_reset();
    for (;;) {
	int	    c;

	/* Fetch next character. */
	if (tp < buf_end)
	    c = *tp++;
	else {
	    c = lookahead();
	    if (c == EOF)
		break;
	}
	/* Handle comments. */
	if (in_comment) {
	    if (lastc == '*' && c == '/')
		in_comment = false;
	} else if (lastc == '/' && c == '*' && !in_slash_comment)
	    in_comment = true;
	else if (in_slash_comment) {
	    if (c == '\n')
		in_slash_comment = false;
	} else if (lastc == '/' && c == '/')
	    in_slash_comment = true;
	/* Count nested parens properly. */
	else if (c == '(')
	    paren_depth++;
	else if (c == ')') {
	    paren_depth--;
	    /*
	     * If we find unbalanced parens, we must have started inside a
	     * declaration.
	     */
	    if (paren_depth < 0)
		return false;
	} else if (paren_depth == 0) {
	    /* We are outside any parentheses or comments. */
	    if (c == '{')
		return true;
	    else if (c == ';' || c == ',')
		return false;
	}
	lastc = c;
    }
    /* Hit EOF --- for lack of anything better, assume "not a definition". */
    return false;
}

int
lexi(struct parser_state *state)
{
    int         unary_delim;	/* this is set to 1 if the current token
				 * forces a following operator to be unary */
    int         code;		/* internal code to be returned */
    char        qchar;		/* the delimiter character for a string */

    e_token = s_token;		/* point to start of place to save token */
    unary_delim = false;
    state->col_1 = state->last_nl;	/* tell world that this token started
					 * in column 1 iff the last thing
					 * scanned was a newline */
    state->last_nl = false;

    while (*buf_ptr == ' ' || *buf_ptr == '\t') {	/* get rid of blanks */
	state->col_1 = false;	/* leading blanks imply token is not in column
				 * 1 */
	if (++buf_ptr >= buf_end)
	    fill_buffer();
    }

    /* Scan an alphanumeric token */
    if (chartype[*buf_ptr & 127] == alphanum ||
	(buf_ptr[0] == '.' && isdigit((unsigned char)buf_ptr[1]))) {
	/*
	 * we have a character or number
	 */
	struct templ *p;

	if (isdigit((unsigned char)*buf_ptr) ||
	    (buf_ptr[0] == '.' && isdigit((unsigned char)buf_ptr[1]))) {
	    int         seendot = 0,
	                seenexp = 0,
			seensfx = 0;

	    /*
	     * base 2, base 8, base 16:
	     */
	    if (buf_ptr[0] == '0' && buf_ptr[1] != '.') {
		int len;

		if (buf_ptr[1] == 'b' || buf_ptr[1] == 'B')
		    len = strspn(buf_ptr + 2, "01") + 2;
		else if (buf_ptr[1] == 'x' || buf_ptr[1] == 'X')
		    len = strspn(buf_ptr + 2, "0123456789ABCDEFabcdef") + 2;
		else
		    len = strspn(buf_ptr + 1, "012345678") + 1;
		if (len > 0) {
		    CHECK_SIZE_TOKEN(len);
		    memcpy(e_token, buf_ptr, len);
		    e_token += len;
		    buf_ptr += len;
		}
		else
		    diag2(1, "Unterminated literal");
	    }
	    else		/* base 10: */
		while (1) {
		    if (*buf_ptr == '.') {
			if (seendot)
			    break;
			else
			    seendot++;
		    }
		    CHECK_SIZE_TOKEN(3);
		    *e_token++ = *buf_ptr++;
		    if (!isdigit((unsigned char)*buf_ptr) && *buf_ptr != '.') {
			if ((*buf_ptr != 'E' && *buf_ptr != 'e') || seenexp)
			    break;
			else {
			    seenexp++;
			    seendot++;
			    *e_token++ = *buf_ptr++;
			    if (*buf_ptr == '+' || *buf_ptr == '-')
				*e_token++ = *buf_ptr++;
			}
		    }
		}

	    while (1) {
		CHECK_SIZE_TOKEN(2);
		if (!(seensfx & 1) && (*buf_ptr == 'U' || *buf_ptr == 'u')) {
		    *e_token++ = *buf_ptr++;
		    seensfx |= 1;
		    continue;
		}
		if (!(seensfx & 2) && (strchr("fFlL", *buf_ptr) != NULL)) {
		    if (buf_ptr[1] == buf_ptr[0])
		        *e_token++ = *buf_ptr++;
		    *e_token++ = *buf_ptr++;
		    seensfx |= 2;
		    continue;
		}
		break;
	    }
	}
	else
	    while (chartype[*buf_ptr & 127] == alphanum || *buf_ptr == BACKSLASH) {
		/* fill_buffer() terminates buffer with newline */
		if (*buf_ptr == BACKSLASH) {
		    if (*(buf_ptr + 1) == '\n') {
			buf_ptr += 2;
			if (buf_ptr >= buf_end)
			    fill_buffer();
			} else
			    break;
		}
		CHECK_SIZE_TOKEN(1);
		/* copy it over */
		*e_token++ = *buf_ptr++;
		if (buf_ptr >= buf_end)
		    fill_buffer();
	    }
	*e_token = '\0';

	if (s_token[0] == 'L' && s_token[1] == '\0' &&
	      (*buf_ptr == '"' || *buf_ptr == '\''))
	    return (strpfx);

	while (*buf_ptr == ' ' || *buf_ptr == '\t') {	/* get rid of blanks */
	    if (++buf_ptr >= buf_end)
		fill_buffer();
	}
	state->keyword = 0;
	if (state->last_token == structure && !state->p_l_follow) {
				/* if last token was 'struct' and we're not
				 * in parentheses, then this token
				 * should be treated as a declaration */
	    state->last_u_d = true;
	    return (decl);
	}
	/*
	 * Operator after identifier is binary unless last token was 'struct'
	 */
	state->last_u_d = (state->last_token == structure);

	p = bsearch(s_token,
	    specials,
	    sizeof(specials) / sizeof(specials[0]),
	    sizeof(specials[0]),
	    strcmp_type);
	if (p == NULL) {	/* not a special keyword... */
	    char *u;

	    /* ... so maybe a type_t or a typedef */
	    if ((auto_typedefs && ((u = strrchr(s_token, '_')) != NULL) &&
	        strcmp(u, "_t") == 0) || (typename_top >= 0 &&
		  bsearch(s_token, typenames, typename_top + 1,
		    sizeof(typenames[0]), strcmp_type))) {
		state->keyword = 4;	/* a type name */
		state->last_u_d = true;
	        goto found_typename;
	    }
	} else {			/* we have a keyword */
	    state->keyword = p->rwcode;
	    state->last_u_d = true;
	    switch (p->rwcode) {
	    case 7:		/* it is a switch */
		return (swstmt);
	    case 8:		/* a case or default */
		return (casestmt);

	    case 3:		/* a "struct" */
		/* FALLTHROUGH */
	    case 4:		/* one of the declaration keywords */
	    found_typename:
		if (state->p_l_follow) {
		    /* inside parens: cast, param list, offsetof or sizeof */
		    state->cast_mask |= (1 << state->p_l_follow) & ~state->not_cast_mask;
		}
		if (state->last_token == period || state->last_token == unary_op) {
		    state->keyword = 0;
		    break;
		}
		if (p != NULL && p->rwcode == 3)
		    return (structure);
		if (state->p_l_follow)
		    break;
		return (decl);

	    case 5:		/* if, while, for */
		return (sp_paren);

	    case 6:		/* do, else */
		return (sp_nparen);

	    case 10:		/* storage class specifier */
		return (storage);

	    case 11:		/* typedef */
		return (type_def);

	    default:		/* all others are treated like any other
				 * identifier */
		return (ident);
	    }			/* end of switch */
	}			/* end of if (found_it) */
	if (*buf_ptr == '(' && state->tos <= 1 && state->ind_level == 0 &&
	    state->in_parameter_declaration == 0 && state->block_init == 0) {
	    if (is_func_definition(buf_ptr)) {
		strncpy(state->procname, token, sizeof state->procname - 1);
		if (state->in_decl)
		    state->in_parameter_declaration = 1;
		return (funcname);
	    }
	}
	/*
	 * The following hack attempts to guess whether or not the current
	 * token is in fact a declaration keyword -- one that has been
	 * typedefd
	 */
	else if (!state->p_l_follow && !state->block_init &&
	    !state->in_stmt &&
	    ((*buf_ptr == '*' && buf_ptr[1] != '=') ||
		isalpha((unsigned char)*buf_ptr)) &&
	    (state->last_token == semicolon || state->last_token == lbrace ||
		state->last_token == rbrace)) {
	    state->keyword = 4;	/* a type name */
	    state->last_u_d = true;
	    return decl;
	}
	if (state->last_token == decl)	/* if this is a declared variable,
					 * then following sign is unary */
	    state->last_u_d = true;	/* will make "int a -1" work */
	return (ident);		/* the ident is not in the list */
    }				/* end of processing for alphanum character */

    /* Scan a non-alphanumeric token */

    CHECK_SIZE_TOKEN(3);		/* things like "<<=" */
    *e_token++ = *buf_ptr;		/* if it is only a one-character token, it is
				 * moved here */
    *e_token = '\0';
    if (++buf_ptr >= buf_end)
	fill_buffer();

    switch (*token) {
    case '\n':
	unary_delim = state->last_u_d;
	state->last_nl = true;	/* remember that we just had a newline */
	code = (had_eof ? 0 : newline);

	/*
	 * if data has been exhausted, the newline is a dummy, and we should
	 * return code to stop
	 */
	break;

    case '\'':			/* start of quoted character */
    case '"':			/* start of string */
	qchar = *token;
	do {			/* copy the string */
	    while (1) {		/* move one character or [/<char>]<char> */
		if (*buf_ptr == '\n') {
		    diag2(1, "Unterminated literal");
		    goto stop_lit;
		}
		CHECK_SIZE_TOKEN(2);
		*e_token = *buf_ptr++;
		if (buf_ptr >= buf_end)
		    fill_buffer();
		if (*e_token == BACKSLASH) {	/* if escape, copy extra char */
		    if (*buf_ptr == '\n')	/* check for escaped newline */
			++line_no;
		    *++e_token = *buf_ptr++;
		    ++e_token;	/* we must increment this again because we
				 * copied two chars */
		    if (buf_ptr >= buf_end)
			fill_buffer();
		}
		else
		    break;	/* we copied one character */
	    }			/* end of while (1) */
	} while (*e_token++ != qchar);
stop_lit:
	code = ident;
	break;

    case ('('):
    case ('['):
	unary_delim = true;
	code = lparen;
	break;

    case (')'):
    case (']'):
	code = rparen;
	break;

    case '#':
	unary_delim = state->last_u_d;
	code = preesc;
	break;

    case '?':
	unary_delim = true;
	code = question;
	break;

    case (':'):
	code = colon;
	unary_delim = true;
	break;

    case (';'):
	unary_delim = true;
	code = semicolon;
	break;

    case ('{'):
	unary_delim = true;

	/*
	 * if (state->in_or_st) state->block_init = 1;
	 */
	/* ?	code = state->block_init ? lparen : lbrace; */
	code = lbrace;
	break;

    case ('}'):
	unary_delim = true;
	/* ?	code = state->block_init ? rparen : rbrace; */
	code = rbrace;
	break;

    case 014:			/* a form feed */
	unary_delim = state->last_u_d;
	state->last_nl = true;	/* remember this so we can set 'state->col_1'
				 * right */
	code = form_feed;
	break;

    case (','):
	unary_delim = true;
	code = comma;
	break;

    case '.':
	unary_delim = false;
	code = period;
	break;

    case '-':
    case '+':			/* check for -, +, --, ++ */
	code = (state->last_u_d ? unary_op : binary_op);
	unary_delim = true;

	if (*buf_ptr == token[0]) {
	    /* check for doubled character */
	    *e_token++ = *buf_ptr++;
	    /* buffer overflow will be checked at end of loop */
	    if (state->last_token == ident || state->last_token == rparen) {
		code = (state->last_u_d ? unary_op : postop);
		/* check for following ++ or -- */
		unary_delim = false;
	    }
	}
	else if (*buf_ptr == '=')
	    /* check for operator += */
	    *e_token++ = *buf_ptr++;
	else if (*buf_ptr == '>') {
	    /* check for operator -> */
	    *e_token++ = *buf_ptr++;
	    unary_delim = false;
	    code = unary_op;
	    state->want_blank = false;
	}
	break;			/* buffer overflow will be checked at end of
				 * switch */

    case '=':
	if (state->in_or_st)
	    state->block_init = 1;
#ifdef undef
	if (chartype[*buf_ptr & 127] == opchar) {	/* we have two char assignment */
	    e_token[-1] = *buf_ptr++;
	    if ((e_token[-1] == '<' || e_token[-1] == '>') && e_token[-1] == *buf_ptr)
		*e_token++ = *buf_ptr++;
	    *e_token++ = '=';	/* Flip =+ to += */
	    *e_token = 0;
	}
#else
	if (*buf_ptr == '=') {/* == */
	    *e_token++ = '=';	/* Flip =+ to += */
	    buf_ptr++;
	    *e_token = 0;
	}
#endif
	code = binary_op;
	unary_delim = true;
	break;
	/* can drop thru!!! */

    case '>':
    case '<':
    case '!':			/* ops like <, <<, <=, !=, etc */
	if (*buf_ptr == '>' || *buf_ptr == '<' || *buf_ptr == '=') {
	    *e_token++ = *buf_ptr;
	    if (++buf_ptr >= buf_end)
		fill_buffer();
	}
	if (*buf_ptr == '=')
	    *e_token++ = *buf_ptr++;
	code = (state->last_u_d ? unary_op : binary_op);
	unary_delim = true;
	break;

    case '*':
	unary_delim = true;
	if (!state->last_u_d) {
	    if (*buf_ptr == '=')
		*e_token++ = *buf_ptr++;
	    code = binary_op;
	    break;
	}
	while (*buf_ptr == '*' || isspace((unsigned char)*buf_ptr)) {
	    if (*buf_ptr == '*') {
		CHECK_SIZE_TOKEN(1);
		*e_token++ = *buf_ptr;
	    }
	    if (++buf_ptr >= buf_end)
		fill_buffer();
	}
	code = unary_op;
	break;

    default:
	if (token[0] == '/' && *buf_ptr == '*') {
	    /* it is start of comment */
	    *e_token++ = '*';

	    if (++buf_ptr >= buf_end)
		fill_buffer();

	    code = comment;
	    unary_delim = state->last_u_d;
	    break;
	}
	while (*(e_token - 1) == *buf_ptr || *buf_ptr == '=') {
	    /*
	     * handle ||, &&, etc, and also things as in int *****i
	     */
	    CHECK_SIZE_TOKEN(1);
	    *e_token++ = *buf_ptr;
	    if (++buf_ptr >= buf_end)
		fill_buffer();
	}
	code = (state->last_u_d ? unary_op : binary_op);
	unary_delim = true;


    }				/* end of switch */
    if (buf_ptr >= buf_end)	/* check for input buffer empty */
	fill_buffer();
    state->last_u_d = unary_delim;
    CHECK_SIZE_TOKEN(1);
    *e_token = '\0';		/* null terminate the token */
    return (code);
}

void
alloc_typenames(void)
{

    typenames = (const char **)malloc(sizeof(typenames[0]) *
        (typename_count = 16));
    if (typenames == NULL)
	err(1, NULL);
}

void
add_typename(const char *key)
{
    int comparison;
    const char *copy;

    if (typename_top + 1 >= typename_count) {
	typenames = realloc((void *)typenames,
	    sizeof(typenames[0]) * (typename_count *= 2));
	if (typenames == NULL)
	    err(1, NULL);
    }
    if (typename_top == -1)
	typenames[++typename_top] = copy = strdup(key);
    else if ((comparison = strcmp(key, typenames[typename_top])) >= 0) {
	/* take advantage of sorted input */
	if (comparison == 0)	/* remove duplicates */
	    return;
	typenames[++typename_top] = copy = strdup(key);
    }
    else {
	int p;

	for (p = 0; (comparison = strcmp(key, typenames[p])) > 0; p++)
	    /* find place for the new key */;
	if (comparison == 0)	/* remove duplicates */
	    return;
	memmove(&typenames[p + 1], &typenames[p],
	    sizeof(typenames[0]) * (++typename_top - p));
	typenames[p] = copy = strdup(key);
    }

    if (copy == NULL)
	err(1, NULL);
}
