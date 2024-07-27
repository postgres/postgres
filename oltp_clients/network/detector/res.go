package detector

import (
	"FC/configs"
	"fmt"
	"sync"
)

const (
	UnDecided = 0
	Commit    = 1
	Abort     = 2
)

type KvRes struct { // No lost res, it is handled at the DBMS.
	TID        int
	ID         string
	Persisted  bool
	VoteCommit bool //VoteCommit if the KV voted to commit.
	IsCommit   bool //IsCommit if the KV shard decide to commit,
}

func NewKvRes(id int, cid string) *KvRes {
	res := &KvRes{
		ID:  cid,
		TID: id,
	}
	res.Clear()
	return res
}

func (c *KvRes) Committed() bool {
	return c.IsCommit
}

func (c *KvRes) Clear() {
	c.IsCommit = false
	c.VoteCommit = false
}

func (c *KvRes) SetSelfResult(vote bool, commit bool, persist bool) {
	c.IsCommit = commit
	c.VoteCommit = vote
	c.Persisted = persist
}

// KvResMakeLost make a lost item with decision
func KvResMakeLost(decision bool) *KvRes {
	return &KvRes{
		TID:        0, // TID = 0 means the result is lost.
		VoteCommit: decision,
		IsCommit:   decision,
	}
}

// KvResult the result container for the Level state manager
type KvResult struct {
	mu           *sync.Mutex
	N            int
	NShard       int
	Decided      int
	KvIDs        []int
	ProtocolRes  []*KvRes
	noCrashed    map[string]bool
	voteCommit   int
	decideCommit int
	crashedCnt   int
	Decision     int // 0 for null, 1 for commit, 2 for abort.
}

func (c *KvResult) String() string {
	return fmt.Sprintf("Result - N:[%d];N:[%d];VoteCommit:[%d];decideCommit:[%d];decided:[%d];decision:[%d]", c.N,
		c.NShard, c.voteCommit, c.decideCommit, c.Decided, c.Decision)
}

func NewKvResult(nShard int) *KvResult {
	res := &KvResult{}
	res.Init(nShard)
	return res
}

// Init initialize the mockkv result
func (re *KvResult) Init(nShard int) {
	re.NShard = nShard
	re.voteCommit = 0
	re.decideCommit = 0
	re.crashedCnt = 0
	re.ProtocolRes = make([]*KvRes, nShard)
	re.noCrashed = make(map[string]bool)
	re.KvIDs = make([]int, nShard)
	re.N = 0
	re.Decision = UnDecided
	re.mu = &sync.Mutex{}
	re.Decided = 0
}

func (re *KvResult) DecideSomeCommit() bool {
	return re.decideCommit > 0
}

func (re *KvResult) DecideAllCommit() bool {
	return re.NShard == re.decideCommit
}

// AppendFinished check if we have finished appending the results from all shards.
func (re *KvResult) AppendFinished() bool {
	return re.NShard == re.N
}

// Append append a KvRes entry to the result.
func (re *KvResult) Append(res *KvRes) bool {
	re.mu.Lock()
	defer re.mu.Unlock()
	i := re.N
	if i >= re.NShard {
		return configs.Assert(false, "append in KvRes reaches out of limit")
	}
	// It is not maintained now.
	if res.Persisted {
		re.Decided++
		if res.IsCommit {
			re.Decision = Commit
		} else {
			re.Decision = Abort
		}
	}
	re.KvIDs[i] = res.TID
	re.ProtocolRes[i] = res
	if res.TID != 0 {
		re.noCrashed[res.ID] = true
	}
	if res.IsCommit {
		re.decideCommit++
	}
	if res.VoteCommit {
		re.voteCommit++
	}
	re.N++
	return true
}

func (re *KvResult) ReturnPersisted() bool {
	return re.N == re.Decided
}

func (re *KvResult) AllPersisted() bool {
	return re.NShard == re.Decided
}

// Correct return if the participant work correctly.
func (re *KvResult) Correct(level Level) bool {
	if level == NoCFNoNF {
		return re.AllPersisted()
	} else {
		return (re.Decision == Abort && re.ReturnPersisted()) || (re.NShard == re.voteCommit && re.Decision == UnDecided)
	}
}

// VoteAllCommit return if all the shards decide to commit.
func (re *KvResult) VoteAllCommit() bool {
	return re.NShard == re.voteCommit
}

func (re *KvResult) voteSomeCommit() bool {
	return !re.VoteAllCommit() && re.voteCommit > 0
}

// decideAllCommit return if all the shards decide to commit.
func (re *KvResult) decideAllCommit() bool {
	return re.NShard == re.decideCommit
}

func (re *KvResult) decideSomeCommit() bool {
	return !re.decideAllCommit() && re.decideCommit > 0
}

func (re *KvResult) detectCrashFailure(shards []string) map[string]bool {
	re.crashedCnt = 0
	res := make(map[string]bool)
	for _, p := range shards {
		if !re.noCrashed[p] {
			res[p] = true
			re.crashedCnt++
		}
	}
	return res
}

// Analysis analysis the result of an atomic commit (bool CrashFailure, bool NetworkFailure, error).
// Property 4.5 (All broadcast) assumed to be held.
// The crash failure should be handled in coordinator before !!!.
func (re *KvResult) Analysis(shards []string, level Level) (map[string]bool, bool) {
	crashFailure := re.detectCrashFailure(shards)
	if level == NoCFNoNF {
		if re.crashedCnt == 0 && !re.AllPersisted() {
			return crashFailure, true
		}
	}
	if level == CFNoNF {
		//		if !re.ReturnPersisted() && re.Decision == Abort {
		//			return crashFailure, true
		//		}
		if re.VoteAllCommit() && !re.decideAllCommit() {
			return crashFailure, true
		}
	}
	return crashFailure, false
}
