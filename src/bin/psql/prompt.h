/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright 2000 by PostgreSQL Global Development Group
 *
 * $Header: /cvsroot/pgsql/src/bin/psql/prompt.h,v 1.7 2000/02/16 13:15:26 momjian Exp $
 */
#ifndef PROMPT_H
#define PROMPT_H

typedef enum _promptStatus
{
	PROMPT_READY,
	PROMPT_CONTINUE,
	PROMPT_COMMENT,
	PROMPT_SINGLEQUOTE,
	PROMPT_DOUBLEQUOTE,
    PROMPT_PAREN,
	PROMPT_COPY
}			promptStatus_t;

char *get_prompt(promptStatus_t status);

#endif	 /* PROMPT_H */
