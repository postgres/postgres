#ifndef PROMPT_H
#define PROMPT_H

#include "settings.h"

typedef enum _promptStatus {
    PROMPT_READY,
    PROMPT_CONTINUE,
    PROMPT_COMMENT,
    PROMPT_SINGLEQUOTE,
    PROMPT_DOUBLEQUOTE,
    PROMPT_COPY
} promptStatus_t;

const char *
get_prompt(PsqlSettings *pset, promptStatus_t status);


#endif /* PROMPT_H */
