#include "postgres.h"

#include "access/transam.h"
#include "access/xact.h"
#include "access/twophase.h"
#include "pgstat.h"
#include "storage/spin.h"
#include "storage/rl_policy.h"
#include "utils/memutils.h"

void after_lock(int i, bool is_read);
void before_lock(int i, bool is_read);
static uint64_t get_cur_time_ns();

LockFeature* LockFeatureVec = NULL;
TrainingState* RLState = NULL;

static bool starts_with(const char *str, const char *pre) {
    return strncmp(pre, str, strlen(pre)) == 0;
}

void init_global_feature_collector()
{
    printf("2PL lock graph initialized (new)\n");
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

    RLState->step ++;
    MyProc->rank = get_policy(tid);

    XactLockStrategy = LOCK_2PL;
    XactIsoLevel = XACT_READ_COMMITTED;


    Assert((!IsolationIsSerializable() && !IsolationNeedLock()) || IsolationLearnCC()
           || XactLockStrategy == DefaultXactLockStrategy || IsolationIsSerializable());
}

void report_xact_result(bool is_commit, uint32 xact_id)
{
    if (SKIP_XACT(xact_id)) return;
    if (!IsolationLearnCC()) return;
}

#define RL_PREDICT_HEADER 0
#define RL_TERMINATE_HEADER 1

void init_rl_state(uint32 xact_id)
{
    RLState = (TrainingState*) MemoryContextAlloc(TopTransactionContext, sizeof (TrainingState));
    RLState->cur_xact_id = xact_id;
#if MODEL_REMOTE == 1
#endif
    RLState->step = 0;
    RLState->k = 0;
    refresh_lock_strategy();
}

double get_policy(uint32 xact_id)
{
#if MODEL_REMOTE == 1
#else
#endif
//    print_current_state(xact_id);
    return 0;
}

void print_current_state(uint32 xact_id)
{
#if MODEL_REMOTE == 1
    printf("[xact:%d, k:%d-%d-%d-%d-%d-%d-%d, block:%.2f-%.2f-%.2f-%.2f, r=%.2f, max_wait=%.2f], the action is %d\n",
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
            RLState->block_info[2],
            RLState->block_info[3],
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
    fprintf(filePtr, "[xact:%d, step:%d, k:%d]\n",
            RLState->cur_xact_id,
            RLState->step,
            RLState->k);
    fclose(filePtr);
#endif
}

void before_lock(int i, bool is_read)
{
    if (!IsolationLearnCC()) return;
//    SpinLockAcquire(&LockFeatureVec[i].mutex);
//    SpinLockRelease(&LockFeatureVec[i].mutex);
    refresh_lock_strategy();
}

void after_lock(int i, bool is_read)
{
    if (!IsolationLearnCC()) return;
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
    SpinLockRelease(&LockFeatureVec[i].mutex);
}