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

// LockFeatureData regard the feature for a tuples grouped by hash.
typedef struct GlobalLockFeatureData {
    double avg_free_time;
    double utility;
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
    uint64_t xact_start_ts;
    uint64_t last_lock_time;
    double avg_expected_wait;
    double last_reward;
    uint32 cur_xact_id; // for validation propose.
    uint16 conflicts[7];
    uint16 block_info[2];
    int action;
} TrainingState;

extern void init_global_feature_collector();
extern void report_intention(uint32 rid, uint32 pgid, uint16 offset, bool is_read, bool is_release);
extern void report_conflict(uint32 rid, uint32 pgid, uint16 offset, bool is_read, bool is_release);
extern void init_rl_state(uint32 xact_id);
extern int rl_next_action(uint32 xact_id);
extern void print_current_state(uint32 xact_id);
extern void refresh_lock_strategy();
extern void finish_rl_process(uint32 xact_id, bool is_commit);
extern void report_xact_result(bool is_commit, uint32 xact_id);

#endif //POSTGRES_RL_POLICY_H
