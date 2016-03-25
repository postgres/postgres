/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright (c) 2000-2016, PostgreSQL Global Development Group
 *
 * src/bin/psql/prompt.h
 */
#ifndef PROMPT_H
#define PROMPT_H

/* enum promptStatus_t is now defined by psqlscan.h */
#include "fe_utils/psqlscan.h"

char	   *get_prompt(promptStatus_t status);

#endif   /* PROMPT_H */
