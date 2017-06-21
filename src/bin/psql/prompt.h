/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright (c) 2000-2017, PostgreSQL Global Development Group
 *
 * src/bin/psql/prompt.h
 */
#ifndef PROMPT_H
#define PROMPT_H

/* enum promptStatus_t is now defined by psqlscan.h */
#include "fe_utils/psqlscan.h"
#include "conditional.h"

char	   *get_prompt(promptStatus_t status, ConditionalStack cstack);

#endif							/* PROMPT_H */
