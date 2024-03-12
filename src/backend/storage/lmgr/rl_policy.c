#include "postgres.h"

#include "access/transam.h"
#include "access/xact.h"
#include "access/twophase.h"
#include "pgstat.h"
#include "storage/spin.h"
#include "storage/rl_policy.h"
#include "utils/memutils.h"

#define LOG_LOCK_FEATURE 5
#define LOCK_FEATURE_LEN (1<<LOG_LOCK_FEATURE)
#define LOCK_FEATURE_MASK (LOCK_FEATURE_LEN-1)
#define REL_ID_MULTI 13
#define MOVING_AVERAGE_RATE 0.8
#define LOCK_KEY(rid, pgid, offset) ((pgid) * 4096 + (offset) + (rid) * REL_ID_MULTI)

void print_current_state(uint32 xact_id);
int rl_next_action(uint32 xact_id);
void finish_rl_process(uint32 xact_id, bool is_commit);
void init_rl_state(uint32 xact_id);
void report_xact_result(bool is_commit, uint32 xact_id);
void refresh_lock_strategy();
void init_global_feature_collector();
void report_conflict(uint32 rid, uint32 pgid, uint16 offset, bool is_read, bool is_release);
void report_intention(uint32 rid, uint32 pgid, uint16 offset, bool is_read, bool is_release);

void after_lock(int i, bool is_read);
void before_lock(int i, bool is_read);
static uint64_t get_cur_time_ns();

LockFeature* LockFeatureVec = NULL;
TrainingState* RLState = NULL;

//#define IS_SYS_TABLE(rel) (starts_with(rel, "pg_") || starts_with(rel, "sql_"))

static bool starts_with(const char *str, const char *pre) {
    return strncmp(pre, str, strlen(pre)) == 0;
}


#define ALG_NUM 12
#define NUM_OF_SYS_XACTS 1
#define SKIP_XACT(tid) ((tid) <= NUM_OF_SYS_XACTS)
#define SEC_TO_NS(sec) ((sec)*1000000000)
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
#define READ_FACTOR 0.5
#define ABORT_PENALTY (-10000.0)
#define NS_TO_US 1000.0
#define FEATURE_MMAP_SIZE 32
#define MODEL_REMOTE 1


// lock strategy, isolation level, deadlock detection interval (global), lock timeout.
//
// Deadlocks are situations where transactions are waiting on each other in a cycle,
// and no progress can be made without intervention. Lock contention,
// on the other hand, happens when one transaction has to wait for locks held by another,
// but progress is still possible once the locks are released.
//
// For deadlock, a value of 1 second is a compromise between detecting and resolving deadlocks promptly and not
// performing the detection so frequently that it becomes a performance issue itself. However, in a system where
// transactions are typically very short, and lock contention is more common, a shorter DL timeout might be justified.
// This period needs to be long enough to allow most transactions to complete
// without triggering unnecessary deadlock checks, thus we make it larger than 100ms.
//
// For lock timeout, if the system is under high load, a shorter lock_timeout can help in quickly resolving lock contention,
// ensuring that no single transaction can block others for too long.
const int alg_list[ALG_NUM][4] = {
        {LOCK_2PL, XACT_READ_COMMITTED, 1000, 0},
        {LOCK_2PL, XACT_READ_COMMITTED, 1000, 1000},
        {LOCK_2PL, XACT_READ_COMMITTED, 1000, 100},
        {LOCK_2PL, XACT_READ_COMMITTED, 1000, 10},
        {LOCK_2PL, XACT_READ_COMMITTED, 1000, 1},
        {LOCK_2PL, XACT_READ_COMMITTED, 100, 0},
        {LOCK_2PL, XACT_READ_COMMITTED, 100, 1000},
        {LOCK_2PL, XACT_READ_COMMITTED, 100, 100},
        {LOCK_2PL, XACT_READ_COMMITTED, 100, 10},
        {LOCK_2PL, XACT_READ_COMMITTED, 100, 1},
        // 10 types of waiting policy
        {LOCK_2PL, XACT_READ_COMMITTED, -1, -1},
        // a special sign: stop learning.
        {LOCK_ASSERT_ABORT, XACT_READ_COMMITTED, 1000, 0}
        // the worst case: stop now.
};

void init_global_feature_collector()
{
    printf("2PL lock graph initialized\n");
    LockFeatureVec = ShmemAllocUnlocked(sizeof (LockFeature) * LOCK_FEATURE_LEN);
    for (int i=0;i<LOCK_FEATURE_LEN;i++)
    {
        SpinLockInit(&LockFeatureVec[i].mutex);
        LockFeatureVec[i].read_cnt = 0;
        LockFeatureVec[i].avg_free_time = 0;
        LockFeatureVec[i].read_intention_cnt = 0;
        LockFeatureVec[i].write_cnt = 0;
        LockFeatureVec[i].write_intention_cnt = 0;
        LockFeatureVec[i].utility = 1.0;
    }
}

static uint64_t get_cur_time_ns()
{
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return SEC_TO_NS((uint64_t)ts.tv_sec) + (uint64_t)ts.tv_nsec;
}

void refresh_lock_strategy()
{
    uint32 tid = MyProc->lxid;
    Assert(RLState != NULL);
    Assert(RLState->cur_xact_id == tid);

    if (SKIP_XACT(tid)) // skip system transactions.
        return;


    if (!IsolationLearnCC())
        return;

    RLState->action = rl_next_action(tid);
    RLState->last_reward = 0;

    XactIsoLevel = alg_list[RLState->action][0];
    XactLockStrategy = alg_list[RLState->action][1];
    DeadlockTimeout = alg_list[RLState->action][2];
    LockTimeout = alg_list[RLState->action][3];


    Assert((!IsolationIsSerializable() && !IsolationNeedLock()) || IsolationLearnCC()
           || XactLockStrategy == DefaultXactLockStrategy || IsolationIsSerializable());

    if (XactLockStrategy == LOCK_ASSERT_ABORT)
    {
        ereport(ERROR,
                (errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
                        errmsg("could not serialize access due to cc strategy"),
                        errdetail_internal("Reason code: Asserted abort by AdjustTransaction."),
                        errhint("The transaction might succeed if retried.")));
    }
}

void report_xact_result(bool is_commit, uint32 xact_id)
{
    if (SKIP_XACT(xact_id)) return;
    if (!IsolationLearnCC())
        return;
    finish_rl_process(xact_id, is_commit);
}

#define RL_PREDICT_HEADER 0
#define RL_TERMINATE_HEADER 1

void init_rl_state(uint32 xact_id)
{
    RLState = (TrainingState*) MemoryContextAlloc(TopTransactionContext, sizeof (TrainingState));
    RLState->cur_xact_id = xact_id;
#if MODEL_REMOTE == 1
#endif
    RLState->action = -1;
    RLState->last_reward = 0;
    RLState->xact_start_ts = get_cur_time_ns();
    memset(RLState->conflicts, 0, sizeof(RLState->conflicts));
    memset(RLState->block_info, 0, sizeof(RLState->block_info));
    RLState->avg_expected_wait = 0;
    refresh_lock_strategy();
}

void finish_rl_process(uint32 xact_id, bool is_commit)
{
    FILE *filePtr = fopen("eposide.txt", "a");
    double time_span;
    if (filePtr == NULL)
    {
        printf("Error opening file.\n");
        return;
    }
    time_span = (double) (get_cur_time_ns() - RLState->last_lock_time) / NS_TO_US;
    Assert(RLState != NULL);
    Assert(RLState->cur_xact_id == xact_id);
    RLState->last_reward = is_commit ? 1.0 : ABORT_PENALTY;
    RLState->last_reward -= 1;
    RLState->last_reward -= time_span * RLState->block_info[READ_OPT] * READ_FACTOR;
    RLState->last_reward -= time_span * RLState->block_info[UPDATE_OPT];

#if MODEL_REMOTE == 1
#else
    fprintf(filePtr, "[xact:%d, reward=%f], action=%d\n",
           xact_id,
           RLState->last_reward,
           RLState->action);
    fclose(filePtr);
#endif
}



int rl_next_action(uint32 xact_id)
{
#if MODEL_REMOTE == 1
//    sprintf(RLState->ptr, "GET_Q,%d,%d,%d,%d,%d,%d,%d,%d,%d,%.5f,%.5f$",
//            RLState->conflicts[0],
//            RLState->conflicts[1],
//            RLState->conflicts[2],
//            RLState->conflicts[3],
//            RLState->conflicts[4],
//            RLState->conflicts[5],
//            RLState->conflicts[6],
//            RLState->block_info[0],
//            RLState->block_info[1],
//            RLState->avg_expected_wait,
//            RLState->last_reward
//            );
#else
    //    RLState->action = random() % ALG_NUM;
//    print_current_state(xact_id);
#endif

    RLState->action = random() % ALG_NUM;
    print_current_state(xact_id);
    Assert(RLState->action >= 0 && RLState->action < ALG_NUM);
    return RLState->action;
}

void print_current_state(uint32 xact_id)
{
    FILE *filePtr = fopen("episode.txt", "a");
    if (filePtr == NULL)
    {
        printf("Error opening file.\n");
        return;
    }
    Assert(RLState != NULL);
    Assert(RLState->cur_xact_id == xact_id);
    fprintf(filePtr, "[xact:%d, k:%d-%d-%d-%d-%d-%d-%d, block:%d-%d, r=%.5f, max_wait=%.5f], the action is %d\n",
            RLState->cur_xact_id,
            RLState->conflicts[0],
            RLState->conflicts[1],
            RLState->conflicts[2],
            RLState->conflicts[3],
            RLState->conflicts[4],
            RLState->conflicts[5],
            RLState->conflicts[6],
            RLState->block_info[0],
            RLState->block_info[1],
            RLState->last_reward,
            RLState->avg_expected_wait,
            RLState->action);
    fclose(filePtr);
}

void before_lock(int i, bool is_read)
{
    if (!IsolationLearnCC()) return;
    SpinLockAcquire(&LockFeatureVec[i].mutex);
    RLState->avg_expected_wait = LockFeatureVec[i].avg_free_time;
    if (is_read)
    {
        RLState->conflicts[RW_INTENTION] = LockFeatureVec[i].write_intention_cnt;
        RLState->conflicts[WR_INTENTION] = 0;
        RLState->conflicts[WW_INTENTION] = 0;
        RLState->conflicts[RW_CONFLICT] = LockFeatureVec[i].write_cnt;
        RLState->conflicts[WW_CONFLICT] = 0;
        RLState->conflicts[WR_CONFLICT] = 0;
    }
    else
    {
        RLState->conflicts[RW_INTENTION] = 0;
        RLState->conflicts[WR_INTENTION] = LockFeatureVec[i].read_intention_cnt;
        RLState->conflicts[WW_INTENTION] = LockFeatureVec[i].write_intention_cnt;
        RLState->conflicts[RW_CONFLICT] = 0;
        RLState->conflicts[WW_CONFLICT] = LockFeatureVec[i].write_cnt;
        RLState->conflicts[WR_CONFLICT] = LockFeatureVec[i].read_cnt;
    }
    SpinLockRelease(&LockFeatureVec[i].mutex);
    RLState->last_lock_time = get_cur_time_ns();
    refresh_lock_strategy();
}

void after_lock(int i, bool is_read)
{
    uint64_t now;
    double time_span;
    if (!IsolationLearnCC()) return;
    now = get_cur_time_ns();
    time_span =  (double)(now - RLState->last_lock_time) / NS_TO_US;
    RLState->last_lock_time = now;
    if (is_read)
        RLState->block_info[READ_OPT] ++;
    else
        RLState->block_info[UPDATE_OPT] ++;

    RLState->last_reward -= 1;
    RLState->last_reward -= time_span * RLState->block_info[READ_OPT] * READ_FACTOR;
    RLState->last_reward -= time_span * RLState->block_info[UPDATE_OPT];
}


void report_intention(uint32 rid, uint32 pgid, uint16 offset, bool is_read, bool is_release)
{
    int i, cmd;
    if (!IsolationLearnCC()) return;
    i = (int)(LOCK_KEY(rid, pgid, offset) & LOCK_FEATURE_MASK);
    cmd = is_release? -1:1;
    Assert(i >= 0 && i < LOCK_FEATURE_LEN);
    if (!is_release) before_lock(i, is_read);
    SpinLockAcquire(&LockFeatureVec[i].mutex);
    if (is_read)
        LockFeatureVec[i].read_intention_cnt += cmd;
    else
        LockFeatureVec[i].write_intention_cnt += cmd;
    SpinLockRelease(&LockFeatureVec[i].mutex);
    if (is_release) after_lock(i, is_read);
}

void report_conflict(uint32 rid, uint32 pgid, uint16 offset, bool is_read, bool is_release)
{
    int i, off;
    if (!IsolationLearnCC()) return;
    i = (int)(LOCK_KEY(rid, pgid, offset) & LOCK_FEATURE_MASK);
    off = is_release? -1:1;

    Assert(i >= 0 && i < LOCK_FEATURE_LEN);
    SpinLockAcquire(&LockFeatureVec[i].mutex);
    if (is_read) LockFeatureVec[i].read_cnt += off;
    else LockFeatureVec[i].write_cnt += off;
    if (is_release)
    {
        double time_span_lag = (double)(get_cur_time_ns() - RLState->xact_start_ts) / NS_TO_US;
        LockFeatureVec[i].utility = LockFeatureVec[i].utility * MOVING_AVERAGE_RATE +
                                    (1 - MOVING_AVERAGE_RATE) * (IsTransactionUseful() ? 1.0 : -100.0);
        if (LockFeatureVec[i].avg_free_time == 0)
            LockFeatureVec[i].avg_free_time = time_span_lag;
        else
            LockFeatureVec[i].avg_free_time = LockFeatureVec[i].avg_free_time * MOVING_AVERAGE_RATE +
                                              (1 - MOVING_AVERAGE_RATE) * time_span_lag;
    }
    SpinLockRelease(&LockFeatureVec[i].mutex);
}