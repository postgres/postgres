/*-------------------------------------------------------------------------
 *
 * read.c
 *	  routines to convert a string (legal ascii representation of node) back
 *	  to nodes
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/nodes/read.c,v 1.14 1999/02/13 23:16:01 momjian Exp $
 *
 * HISTORY
 *	  AUTHOR			DATE			MAJOR EVENT
 *	  Andrew Yu			Nov 2, 1994		file creation
 *
 *-------------------------------------------------------------------------
 */
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "postgres.h"
#include "nodes/pg_list.h"
#include "nodes/readfuncs.h"
#include "utils/elog.h"

/*
 * stringToNode -
 *	  returns a Node with a given legal ascii representation
 */
void *
stringToNode(char *str)
{
	void	   *retval;

	lsptok(str, NULL);			/* set the string used in lsptok */
	retval = nodeRead(true);	/* start reading */

	return retval;
}

/*****************************************************************************
 *
 * the lisp token parser
 *
 *****************************************************************************/

#define RIGHT_PAREN (1000000 + 1)
#define LEFT_PAREN	(1000000 + 2)
#define PLAN_SYM	(1000000 + 3)
#define AT_SYMBOL	(1000000 + 4)
#define ATOM_TOKEN	(1000000 + 5)

/*
 * nodeTokenType -
 *	  returns the type of the node token contained in token.
 *	  It returns one of the following valid NodeTags:
 *		T_Integer, T_Float, T_String
 *	  and some of its own:
 *		RIGHT_PAREN, LEFT_PAREN, PLAN_SYM, AT_SYMBOL, ATOM_TOKEN
 *
 *	  Assumption: the ascii representation is legal
 */
static NodeTag
nodeTokenType(char *token, int length)
{
	NodeTag		retval = 0;

	/*
	 * Check if the token is a number (decimal or integer, positive or
	 * negative
	 */
	if (isdigit(*token) ||
		(length >= 2 && *token == '-' && isdigit(*(token + 1))))
	{

		/*
		 * skip the optional '-' (i.e. negative number)
		 */
		if (*token == '-')
			token++;

		/*
		 * See if there is a decimal point
		 */

		for (; length && *token != '.'; token++, length--);

		/*
		 * if there isn't, token's an int, otherwise it's a float.
		 */

		retval = (*token != '.') ? T_Integer : T_Float;
	}
	else if (isalpha(*token) || *token == '_' ||
			 (token[0] == '<' && token[1] == '>'))
		retval = ATOM_TOKEN;
	else if (*token == '(')
		retval = LEFT_PAREN;
	else if (*token == ')')
		retval = RIGHT_PAREN;
	else if (*token == '@')
		retval = AT_SYMBOL;
	else if (*token == '\"')
		retval = T_String;
	else if (*token == '{')
		retval = PLAN_SYM;
	return retval;
}

/*
 * Works kinda like strtok, except it doesn't put nulls into string.
 *
 * Returns the length in length instead.  The string can be set without
 * returning a token by calling lsptok with length == NULL.
 *
 */
char *
lsptok(char *string, int *length)
{
	static char *local_str;
	char	   *ret_string;

	if (string != NULL)
	{
		local_str = string;
		if (length == NULL)
			return NULL;
	}

	for (; *local_str == ' '
		 || *local_str == '\n'
		 || *local_str == '\t'; local_str++);

	/*
	 * Now pointing at next token.
	 */
	ret_string = local_str;
	if (*local_str == '\0')
		return NULL;
	*length = 1;

	if (*local_str == '"')
	{
		for (local_str++; *local_str != '"'; (*length)++, local_str++)
			;
		(*length)++;
		local_str++;
	}
	/* NULL */
	else if (local_str[0] == '<' && local_str[1] == '>')
	{
		*length = 0;
		local_str += 2;
	}
	else if (*local_str == ')' || *local_str == '(' ||
			 *local_str == '}' || *local_str == '{')
		local_str++;
	else
	{
		for (; *local_str != ' '
			 && *local_str != '\n'
			 && *local_str != '\t'
			 && *local_str != '{'
			 && *local_str != '}'
			 && *local_str != '('
			 && *local_str != ')'; local_str++, (*length)++);
		(*length)--;
	}
	return ret_string;
}

/*
 * This guy does all the reading.
 *
 * Secrets:  He assumes that lsptok already has the string (see below).
 * Any callers should set read_car_only to true.
 */
void *
nodeRead(bool read_car_only)
{
	char	   *token;
	NodeTag		type;
	Node	   *this_value = NULL,
			   *return_value = NULL;
	int			tok_len;
	char		tmp;
	bool		make_dotted_pair_cell = false;

	token = lsptok(NULL, &tok_len);

	if (token == NULL)
		return NULL;

	type = nodeTokenType(token, tok_len);

	switch (type)
	{
		case PLAN_SYM:
			this_value = parsePlanString();
			token = lsptok(NULL, &tok_len);
			if (token[0] != '}')
				return NULL;

			if (!read_car_only)
				make_dotted_pair_cell = true;
			else
				make_dotted_pair_cell = false;
			break;
		case LEFT_PAREN:
			if (!read_car_only)
			{
				List	   *l = makeNode(List);

				lfirst(l) = nodeRead(false);
				lnext(l) = nodeRead(false);
				this_value = (Node *) l;
			}
			else
				this_value = nodeRead(false);
			break;
		case RIGHT_PAREN:
			this_value = NULL;
			break;
		case AT_SYMBOL:
			break;
		case ATOM_TOKEN:
			if (!strncmp(token, "<>", 2))
			{
				this_value = NULL;

				/*
				 * It might be NULL but it is an atom!
				 */
				if (read_car_only)
					make_dotted_pair_cell = false;
				else
					make_dotted_pair_cell = true;
			}
			else
			{
				tmp = token[tok_len];
				token[tok_len] = '\0';
				this_value = (Node *) pstrdup(token);	/* !attention! not a
														 * Node. use with
														 * caution */
				token[tok_len] = tmp;
				make_dotted_pair_cell = true;
			}
			break;
		case T_Float:
			tmp = token[tok_len];
			token[tok_len] = '\0';
			this_value = (Node *) makeFloat(atof(token));
			token[tok_len] = tmp;
			make_dotted_pair_cell = true;
			break;
		case T_Integer:
			tmp = token[tok_len];
			token[tok_len] = '\0';
			this_value = (Node *) makeInteger(atoi(token));
			token[tok_len] = tmp;
			make_dotted_pair_cell = true;
			break;
		case T_String:
			tmp = token[tok_len - 1];
			token[tok_len - 1] = '\0';
			token++;
			this_value = (Node *) makeString(token);	/* !! not strdup'd */
			token[tok_len - 2] = tmp;
			make_dotted_pair_cell = true;
			break;
		default:
			elog(ERROR, "nodeRead: Bad type %d", type);
			break;
	}
	if (make_dotted_pair_cell)
	{
		List	   *l = makeNode(List);

		lfirst(l) = this_value;

		if (!read_car_only)
			lnext(l) = nodeRead(false);
		else
			lnext(l) = NULL;
		return_value = (Node *) l;
	}
	else
		return_value = this_value;
	return return_value;
}
