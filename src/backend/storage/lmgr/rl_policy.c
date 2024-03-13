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
#define READ_FACTOR 0.2
#define ABORT_PENALTY (-2000.0)
#define COMMIT_AWARD 0
#define FEATURE_MMAP_SIZE 32
#define MODEL_REMOTE 0


// small set of action
#define ALG_NUM 7
#define DEFAULT_CC_ALG 1
const int alg_list[ALG_NUM][3] = {
        {LOCK_ASSERT_ABORT, XACT_READ_COMMITTED, 0},
        {LOCK_2PL_NW, XACT_READ_COMMITTED, 0},
        {LOCK_2PL, XACT_READ_COMMITTED, 1},
        {LOCK_2PL, XACT_READ_COMMITTED, 10},
        {LOCK_2PL, XACT_READ_COMMITTED, 100},
        {LOCK_2PL, XACT_READ_COMMITTED, 1000},
        {LOCK_2PL, XACT_READ_COMMITTED, 0},
};


// large set
//#define ALG_NUM 12
//#define DEFAULT_CC_ALG 4
//const int alg_list[ALG_NUM][3] = {
//        {LOCK_ASSERT_ABORT, XACT_READ_COMMITTED, 0},
//        {LOCK_2PL, XACT_READ_COMMITTED, 1},
//        {LOCK_2PL, XACT_READ_COMMITTED, 4},
//        {LOCK_2PL, XACT_READ_COMMITTED, 8},
//        {LOCK_2PL, XACT_READ_COMMITTED, 16},
//        {LOCK_2PL, XACT_READ_COMMITTED, 32},
//        {LOCK_2PL, XACT_READ_COMMITTED, 64},
//        {LOCK_2PL, XACT_READ_COMMITTED, 256},
//        {LOCK_2PL, XACT_READ_COMMITTED, 512},
//        {LOCK_2PL, XACT_READ_COMMITTED, 1024},
//        {LOCK_2PL, XACT_READ_COMMITTED, 2048},
//        {LOCK_2PL, XACT_READ_COMMITTED, 0},
//};

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
    LockTimeout = alg_list[RLState->action][2];


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
    if (!IsolationLearnCC()) return;
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
    RLState->last_lock_time = 0;
    memset(RLState->conflicts, 0, sizeof(RLState->conflicts));
    memset(RLState->block_info, 0, sizeof(RLState->block_info));
    RLState->avg_expected_wait = 0;
    SpinLockInit(&RLState->mutex);
}

void finish_rl_process(uint32 xact_id, bool is_commit)
{
    FILE *filePtr = fopen("episode.txt", "a");
    double time_span;
    if (filePtr == NULL)
    {
        printf("Error opening file.\n");
        return;
    }
    if (RLState->last_lock_time == 0) return;
    Assert(RLState->last_lock_time > 0 || XactLockStrategy == LOCK_ASSERT_ABORT);
    time_span = (double) NS_TO_US(get_cur_time_ns() - RLState->last_lock_time);
    Assert(RLState != NULL);
    Assert(RLState->cur_xact_id == xact_id);
    RLState->last_reward = is_commit ? COMMIT_AWARD : ABORT_PENALTY;
//    RLState->last_reward -= time_span * RLState->block_info[READ_OPT] * READ_FACTOR;
//    RLState->last_reward -= time_span * RLState->block_info[UPDATE_OPT];

#if MODEL_REMOTE == 1
#else
    fprintf(filePtr, "[xact:%d, reward=%.2f], action=%d\n",
           xact_id,
           RLState->last_reward,
           RLState->action);
    fclose(filePtr);
#endif
}



int rl_next_action(uint32 xact_id)
{
#if MODEL_REMOTE == 1
#else
#endif
//    RLState->action = DEFAULT_CC_ALG;
    RLState->action = (int)(random()) % ALG_NUM;
    print_current_state(xact_id);
    Assert(RLState->action >= 0 && RLState->action < ALG_NUM);
    return RLState->action;
}

void print_current_state(uint32 xact_id)
{
#if MODEL_REMOTE == 1
    printf("[xact:%d, k:%d-%d-%d-%d-%d-%d-%d, block:%.2f-%.2f, r=%.2f, max_wait=%.2f], the action is %d\n",
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
#else
    FILE *filePtr = fopen("episode.txt", "a");
    if (filePtr == NULL)
    {
        printf("Error opening file.\n");
        return;
    }
    Assert(RLState != NULL);
    Assert(RLState->cur_xact_id == xact_id);
    fprintf(filePtr, "[xact:%d, k:%d-%d-%d-%d-%d-%d-%d, block:%.2f-%.2f, r=%.2f, max_wait=%.2f], the action is %d\n",
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
#endif
}

void before_lock(int i, bool is_read)
{
    if (!IsolationLearnCC()) return;
    SpinLockAcquire(&LockFeatureVec[i].mutex);
    SpinLockAcquire(&RLState->mutex);   // we enforce the get lock from a single transaction to be executed in serial.
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
    Assert(RLState->last_lock_time > 0);
    time_span =  (double)NS_TO_US(now - RLState->last_lock_time);
    RLState->last_lock_time = now;
    SpinLockAcquire(&LockFeatureVec[i].mutex);
    if (is_read)    // cumulative blocking effect.
        RLState->block_info[READ_OPT] += LockFeatureVec[i].write_intention_cnt;
    else
        RLState->block_info[UPDATE_OPT] +=
                LockFeatureVec[i].write_intention_cnt + LockFeatureVec[i].read_intention_cnt;
    SpinLockRelease(&LockFeatureVec[i].mutex);

    RLState->last_reward -= time_span * RLState->block_info[READ_OPT] * READ_FACTOR;
    // the release of a single read operation will not always results in the preceeding of blocked xact.
    // the block of those transactions shall be attributed to all xacts that holds read lock.
    RLState->last_reward -= time_span * RLState->block_info[UPDATE_OPT];
    SpinLockRelease(&RLState->mutex);
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
        double time_span_lag = (double)NS_TO_US(get_cur_time_ns() - RLState->xact_start_ts);
        if (LockFeatureVec[i].avg_free_time == 0)
            LockFeatureVec[i].avg_free_time = time_span_lag;
        else
            LockFeatureVec[i].avg_free_time = LockFeatureVec[i].avg_free_time * MOVING_AVERAGE_RATE +
                                              (1 - MOVING_AVERAGE_RATE) * time_span_lag;
    }
    SpinLockRelease(&LockFeatureVec[i].mutex);
}