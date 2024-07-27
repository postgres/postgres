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
CachedPolicy* Policy = NULL;
const uint32_t timeout_choices[10] = {1, 4, 8, 16, 64, 128, 256, 512, 1024, 0};
const uint32_t feature_bits[] = {1, 4, 2};
const uint32_t feature_caps[] = {1, 15, 3};

inline uint32_t minUInt32(uint32_t x, uint32_t y) {
    if (x < y) return x;
    return y;
}

inline uint32_t encode_tx_state() {
    uint32_t res = 0, i = 0;
    Assert(RLState->op >= 0 && RLState->op <= 1);
    Assert(RLState->n_r >= 0);
    Assert(RLState->n_w >= 0);
    res |= minUInt32(RLState->op, feature_caps[i]);
    res <<= feature_bits[i++];
    res |= minUInt32(RLState->n_r+RLState->n_w, feature_caps[i]);
//    res <<= feature_bits[i++];
//    Assert(RLState->k >= 0);
//    res |= minUInt32(RLState->k, feature_caps[i]);
    return res;
}

static bool starts_with(const char *str, const char *pre) {
    return strncmp(pre, str, strlen(pre)) == 0;
}

bool load_policy()
{
    FILE* pol_file;
    int length;
    char *token;
    char wait_policy_str[STATE_SPACE + 2];
    char timeout_policy_str[STATE_SPACE + 2];

    printf("starting load policy\n");
    pol_file = fopen("/home/hexiang/Projects/CProj/pg-v12/postgres/policies.txt", "r");
    if (!pol_file) {
        perror("Failed to open file");
        return false;
    }

    printf("starting load wait rank\n");
    if (fgets(wait_policy_str, sizeof(wait_policy_str), pol_file) == NULL) {
        perror("Failed to read wait_policy_str");
        fclose(pol_file);
        return false;
    }

    printf("starting load wait timeout\n");
    if (fgets(timeout_policy_str, sizeof(timeout_policy_str), pol_file) == NULL) {
        perror("Failed to read timeout_policy_str");
        fclose(pol_file);
        return false;
    }
    printf("Policy strings:\n%s\n%s\n", wait_policy_str, timeout_policy_str);
    Assert(strlen(wait_policy_str) == strlen(timeout_policy_str));
    length = (int) strlen(wait_policy_str) - 1;
    while (!isdigit(wait_policy_str[length-1])) length --;
    Assert(length == STATE_SPACE);
    token = strtok(wait_policy_str, " ");
    for (int i = 0; i < length; ++i) {
        Policy->rank[i] = strtod(token, NULL);
        token = strtok(NULL, " ");
        Assert(timeout_policy_str[i] >= '0' && timeout_policy_str[i] <= '9');
        Policy->timeout[i] = timeout_choices[timeout_policy_str[i] - '0'];
    }
    fclose(pol_file);
    printf("finished load policies\n");
    return true;
}

void init_policy_maker()
{
    printf("2PL lock graph initialized (new)\n");
    LockFeatureVec = ShmemAllocUnlocked(sizeof (LockFeature) * LOCK_FEATURE_LEN);
    Policy = ShmemAllocUnlocked(sizeof (CachedPolicy));
    for (int i=0;i<LOCK_FEATURE_LEN;i++)
    {
        SpinLockInit(&LockFeatureVec[i].mutex);
        LockFeatureVec[i].read_cnt = 0;
        LockFeatureVec[i].read_intention_cnt = 0;
        LockFeatureVec[i].write_cnt = 0;
        LockFeatureVec[i].write_intention_cnt = 0;
    }
    Assert(load_policy());
}

static uint64_t get_cur_time_ns()
{
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return SEC_TO_NS((uint64_t)ts.tv_sec) + (uint64_t)ts.tv_nsec;
}

void refresh_lock_strategy()
{
    uint32 tid = MyProc->lxid, state;
    Assert(RLState != NULL);
    Assert(RLState->cur_xact_id == tid);

    if (SKIP_XACT(tid)) // skip system transactions.
        return;


    if (!IsolationLearnCC())
        return;

    state = encode_tx_state();
    Assert(state >= 0 && state < STATE_SPACE);
    MyProc->rank = Policy->rank[state];
    LockTimeout = (int) Policy->timeout[state];
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
    RLState->n_r = 0;
    RLState->n_w = 0;
    RLState->k = 0;
    RLState->op = 0;
    RLState->max_state = 0;
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
            RLState->n_r + RLState->n_w,
            RLState->k);
    fclose(filePtr);
#endif
}

void before_lock(int i, bool is_read)
{
    if (!IsolationLearnCC()) return;
    RLState->op = is_read? 0:1;
    refresh_lock_strategy();
}

void after_lock(int i, bool is_read)
{
    if (!IsolationLearnCC()) return;
    if (is_read) RLState->n_r ++;
    else RLState->n_w ++;
}


void report_intention(uint32 rid, uint32 pgid, uint16 offset, bool is_read, bool is_release)
{
//    int i, cmd;
//    if (!IsolationLearnCC()) return;
//    i = (int)(LOCK_KEY(rid, pgid, offset) & LOCK_FEATURE_MASK);
//    cmd = is_release? -1:1;
//    Assert(i >= 0 && i < LOCK_FEATURE_LEN);
    if (!is_release) before_lock(0, is_read);
//    SpinLockAcquire(&LockFeatureVec[i].mutex);
//    if (is_read)
//        LockFeatureVec[i].read_intention_cnt += cmd;
//    else
//        LockFeatureVec[i].write_intention_cnt += cmd;
//    SpinLockRelease(&LockFeatureVec[i].mutex);
    if (is_release) after_lock(0, is_read);
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