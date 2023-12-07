#include "executor/executor.h"
#include "postgres.h"

#ifndef POSTGRES_CC_H
#define POSTGRES_CC_H

enum CC_ALG {
    NO_WAIT_2PL,
    DL_2PL,
    OCC,
    SSI
};

extern enum CC_ALG default_cc_alg = NO_WAIT_2PL;
#endif //POSTGRES_CC_H
