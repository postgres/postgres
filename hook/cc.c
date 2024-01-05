#include "postgres.h"
#include "optimizer/planner.h"
#include "storage/lmgr.h"
#include "tcop/utility.h"
#include "utils/builtins.h"

PG_MODULE_MAGIC;

static planner_hook_type prev_planner_hook = NULL;

