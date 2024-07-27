#ifndef POSTGRES_RL_POLICY_H
#define POSTGRES_RL_POLICY_H

#ifdef FRONTEND
#error "rl_policy.h may not be included from frontend code"
#endif

#include <access/relation.h>
#include "storage/lockdefs.h"
#include "storage/backendid.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "lib/stringinfo.h"

#define LOG_LOCK_FEATURE 15
#define LOCK_FEATURE_LEN (1<<LOG_LOCK_FEATURE)
#define LOCK_FEATURE_MASK (LOCK_FEATURE_LEN-1)
#define REL_ID_MULTI 13
#define STATE_SPACE 32
#define N_KEY_FEATURES 2
#define MOVING_AVERAGE_RATE 0.8
#define LOCK_KEY(rid, pgid, offset) ((pgid) * 4096 + (offset) + (rid) * REL_ID_MULTI)

typedef struct CachedPolicyData {
    double     rank[STATE_SPACE];
    uint32_t   timeout[STATE_SPACE];
} CachedPolicy;

// LockFeatureData regard the feature for a tuples grouped by hash.
typedef struct GlobalLockFeatureData {
    uint16 read_cnt;
    uint16 write_cnt;
    uint16 read_intention_cnt;
    uint16 write_intention_cnt;
    slock_t mutex;
    unsigned char   padding[3];
} LockFeature;


/* features from local lock graph. */
// k: features that represents the current xact conflict information.
// mu: the expected lock wait time for current transaction to get the lock.
// we have also considered the impact of deadlock abort and unifies them with utility value.
// the number of locks held by current xact. 2 types.

/* features from global lock graph. */
// B: the expected cost for a transaction to be aborted. Since we consider transaction, we use operation number
// for feature.
typedef struct XactState {
    uint32 cur_xact_id; // for validation propose.
    uint16 n_r;
    uint16 n_w;
    uint16 k;
    uint8 op;
    uint32 max_state;
} TrainingState;

extern void init_policy_maker();
extern void report_intention(uint32 rid, uint32 pgid, uint16 offset, bool is_read, bool is_release);
extern void report_conflict(uint32 rid, uint32 pgid, uint16 offset, bool is_read, bool is_release);
extern void init_rl_state(uint32 xact_id);
extern double get_policy(uint32 xact_id);
extern void print_current_state(uint32 xact_id);
extern void refresh_lock_strategy();
//extern void finish_rl_process(uint32 xact_id, bool is_commit);
extern void report_xact_result(bool is_commit, uint32 xact_id);

#define NUM_OF_SYS_XACTS 5
#define SKIP_XACT(tid) ((tid) <= NUM_OF_SYS_XACTS)
#define SEC_TO_NS(sec) ((sec)*1000000000)
#define NS_TO_US(ns) ((ns)/1000.0)
// the intention means the potential conflict dependency caused by parallel requesters. e.e. waiters.
#define RW_INTENTION 0
#define WW_INTENTION 1
#define WR_INTENTION 2
// the conflict means the number of dependency that will cause conflict.
#define RW_CONFLICT 3
#define WR_CONFLICT 4
#define WW_CONFLICT 5
#define READ_OPT 0
#define UPDATE_OPT 1
#define READ_CONTENTION 2
#define UPDATE_CONTENTION 3
#define READ_FACTOR 0.2
#define ABORT_PENALTY (-1000.0)
#define COMMIT_AWARD 100
#define FEATURE_MMAP_SIZE 32
#define MODEL_REMOTE 0
#define IS_SYS_TABLE(rel) (starts_with(rel, "pg_") || starts_with(rel, "sql_"))


// small set of action
#define ALG_NUM 6
#define DEFAULT_CC_ALG 2


#endif //POSTGRES_RL_POLICY_H
