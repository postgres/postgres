package participant

import (
	"FC/configs"
	"FC/network"
	"FC/network/detector"
	"FC/storage"
	"fmt"
	"strconv"
	"sync"
	"sync/atomic"
	"time"
)

// Manager manages the rpc calls From others and maintains the shardKv.
type Manager struct {
	stmt *Context
	// the first dimension is used for multiple shard inside one node.
	Shards      map[string]*storage.Shard
	txnBranches map[string]*sync.Map
	// the messages could come before transaction begins, thus use a separate map
	msgPool map[string]*sync.Map

	// test bits, used to simulate crash failure and network delay.
	broken int32
	nF     int32

	// this the mark used to indicate whether the workload has begun to run from the coordinator side.
	txnComing bool
}

// NewParticipantManager create a new participant manager under stmt.
func NewParticipantManager(stmt *Context, storageSize int) *Manager {
	res := &Manager{
		Shards:      make(map[string]*storage.Shard),
		stmt:        stmt,
		broken:      0,
		nF:          0,
		txnBranches: map[string]*sync.Map{},
		msgPool:     map[string]*sync.Map{},
	}
	if configs.EnableReplication {
		curPosition := 0
		for i := 0; i < configs.NumberOfShards; i++ {
			if stmt.participants[i] == stmt.address {
				curPosition = i
			}
		}
		for i := 0; i < configs.NumberOfReplicas; i++ {
			// the current nodes contains the data for shard [i] [i-1] ... [i-R+1]
			j := stmt.participants[(curPosition-i+configs.NumberOfShards)%configs.NumberOfShards]
			res.Shards[j] = storage.NewKV(j, storageSize, configs.BenchmarkStorage, 0)
			res.msgPool[j] = &sync.Map{}
			res.txnBranches[j] = &sync.Map{}
		}
	} else {
		j := stmt.address
		res.Shards[j] = storage.NewKV(j, storageSize, configs.BenchmarkStorage, 0)
		res.msgPool[j] = &sync.Map{}
		res.txnBranches[j] = &sync.Map{}
	}
	return res
}

func (c *Manager) createIfNotExistTxnBranch(msg *network.CoordinatorGossip) *TXNBranch {
	tx, ok := c.txnBranches[msg.ShardID].Load(msg.TxnID)
	if !ok {
		tx = NewParticipantBranch(msg, c.Shards[msg.ShardID], c)
		tx.(*TXNBranch).beginTime = time.Now()
		tx, ok = c.txnBranches[msg.ShardID].LoadOrStore(msg.TxnID, tx)
		c.txnComing = true
		if !ok {
			if configs.StoredProcedure {
				c.Shards[msg.ShardID].Begin(uint32(msg.TxnID), msg.OptList)
			} else {
				c.Shards[msg.ShardID].Begin(uint32(msg.TxnID), nil)
			}
		}
	}
	return tx.(*TXNBranch)
}

func (c *Manager) mustExistTxnBranch(msg *network.CoordinatorGossip) *TXNBranch {
	tx, ok := c.txnBranches[msg.ShardID].Load(msg.TxnID)
	configs.Assert(ok, "the transaction branch must exist")
	return tx.(*TXNBranch)
}

func (c *Manager) ignoreIfNotExistTxnBranch(msg *network.CoordinatorGossip) *TXNBranch {
	tx, ok := c.txnBranches[msg.ShardID].Load(msg.TxnID)
	if !ok {
		return nil
	} else {
		return tx.(*TXNBranch)
	}
}

func (c *Manager) tryLoadTxnBranch(shard string, tid uint64) *TXNBranch {
	tx, ok := c.txnBranches[shard].Load(tid)
	if !ok {
		return nil
	} else {
		return tx.(*TXNBranch)
	}
}

// PreRead handles the read operations in tx and return results as a map.
func (c *Manager) PreRead(msg *network.CoordinatorGossip) (map[string]*storage.RowData, bool) {
	configs.Assert(!configs.StoredProcedure, "no pre-read shall be called in a store procedure mode")
	return c.createIfNotExistTxnBranch(msg).PreRead()
}

// PreWrite checks if we can get all the locks for write-only transaction tx to ensure ACID.
func (c *Manager) PreWrite(msg *network.CoordinatorGossip) bool {
	tx := c.createIfNotExistTxnBranch(msg)
	tx.OptList = msg.OptList
	tx.pruneRepeatedOperations()
	ok := tx.PreWrite()
	if !ok {
		// without yes vote, the transaction gets directly aborted.
		c.Abort(msg)
		return false
	}
	if msg.Protocol == configs.NoACP {
		// the decisions for single sharded transactions are made instantly.
		return c.Commit(msg)
	}
	return true
}

// PreCommit performs the pre-commit phase of 3PC.
func (c *Manager) PreCommit(msg *network.CoordinatorGossip) bool {
	var tx *TXNBranch
	if msg.Protocol == configs.GPAC {
		tx = c.ignoreIfNotExistTxnBranch(msg)
		if tx == nil {
			// even if the transaction branch has finished, the node can still participant in agreement
			return c.Shards[msg.ShardID].PreCommitAsync(uint32(msg.TxnID))
		}
	} else {
		tx = c.mustExistTxnBranch(msg)
	}
	return tx.PreCommit()
}

// Propose handles the proposal phase of the FCFF algorithm
func (c *Manager) Propose(msg *network.CoordinatorGossip, sent time.Time) *detector.KvRes {
	tx := c.createIfNotExistTxnBranch(msg)
	defer configs.TimeTrack(time.Now(), "Propose", tx.TID)
	configs.Warn(!configs.EnableReplication, "FCff propose does not support replication")
	configs.TPrintf("TXN" + strconv.FormatUint(tx.TID, 10) + ": " + c.stmt.address + " begin propose")
	if msg.Protocol == configs.FCff {
		return tx.ProposeFCFF(msg, sent)
	} else if msg.Protocol == configs.FCcf {
		return tx.ProposeFCCF(msg, sent)
	} else {
		panic("incorrect protocol selected")
		return nil
	}
}

func (c *Manager) Commit(msg *network.CoordinatorGossip) bool {
	tx := c.ignoreIfNotExistTxnBranch(msg)
	//configs.JPrint(msg)
	if msg.Protocol == configs.EasyCommit || msg.Protocol == configs.FCff {
		// transmission and then decide.
		configs.Assert(!configs.EnableReplication, "transmit-and-decide is not supported in replicated setting")
		msg.Protocol = configs.EasyCommit
		c.broadCastVote(msg, 1, msg.ShardID, msg.ParticipantAddresses, time.Time{})
	}
	if tx == nil && configs.EnableReplication {
		// this case can only happen for a replicated protocol when one node fall behind others.
		//configs.Assert(configs.EnableReplication,
		//	fmt.Sprintf("impossible case: the transaction branch %v commit on shard %v without begin", msg.TxnID, msg.ShardID))
		tx = NewParticipantBranch(msg, c.Shards[msg.ShardID], c)
		tx.PreWrite()
	} else if tx == nil {
		// only when transit-before-decide could re-commit transactions, ignore it.
		configs.Assert(msg.Protocol == configs.EasyCommit,
			fmt.Sprintf("impossible case: the transaction branch %v commit on shard %v without begin", msg.TxnID, msg.ShardID))
		return true
	}
	startCommitTime := time.Now()
	ok := tx.Commit()
	if msg.Protocol != configs.NoACP {
		configs.Assert(ok, "current logic should not encounter the transaction commit fail")
	}
	c.clear(msg.ShardID, tx.TID)
	configs.TPrintf("the commit finishes on participant manager %s", c.stmt.address)
	tx.info.IsAbort = !ok
	tx.info.DecideTime = time.Since(startCommitTime)
	c.stmt.stats.Append(tx.info)
	return ok
}

func (c *Manager) Abort(msg *network.CoordinatorGossip) bool {
	tx := c.ignoreIfNotExistTxnBranch(msg)
	if tx == nil {
		configs.Warn(false, "the transaction branch get removed before abort")
		// the transaction has not started on this node, can just ignore.
		return true
	}
	if msg.Protocol == configs.EasyCommit || msg.Protocol == configs.FCff {
		// transmission and then decide.
		configs.Warn(!configs.EnableReplication, "transmit-and-decide currently does not support replication")
		msg.Protocol = configs.EasyCommit
		c.broadCastVote(msg, 0, msg.ShardID, msg.ParticipantAddresses, time.Time{})
	}
	startAbortTime := time.Now()
	ok := tx.Abort()
	configs.Assert(ok, "current logic should not encounter the transaction abort fail")
	c.clear(msg.ShardID, tx.TID)
	configs.TPrintf("TXN"+strconv.FormatUint(msg.TxnID, 10)+": "+"the abort finishes on participant manager %v", msg.ShardID)
	tx.info.IsAbort = true
	tx.info.DecideTime = time.Since(startAbortTime)
	c.stmt.stats.Append(tx.info)
	return ok
}

// checkCanAbort check if the participant has collected enough information to abort the transaction.
func (c *TXNBranch) checkCanAbort() bool {
	for _, op := range c.voteReceived {
		if !op {
			return true
		}
	}
	return false
}

// checkCanCommit check if the participant has collected enough information to commit the transaction.
func (c *TXNBranch) checkCanCommit(expectedYesVoteReceived int) bool {
	if len(c.voteReceived) < expectedYesVoteReceived {
		return false
	}
	for _, op := range c.voteReceived {
		if !op {
			return false
		}
	}
	return true
}

// Propose handles the proposal phase of the FCFF algorithm
func (c *Manager) clear(shard string, txnID uint64) {
	c.txnBranches[shard].Delete(txnID)
	c.msgPool[shard].Delete(txnID)
}

/* test APIs to simulate the system failures */

// Break the interface to inject crash failure.
func (c *Manager) Break() {
	configs.LPrintf(c.stmt.address + " is crashed !!!!")
	atomic.StoreInt32(&c.broken, 1)
	atomic.StoreInt32(&configs.TestCF, 1)
}

// NetBreak the interface to inject network failure.
func (c *Manager) NetBreak() {
	configs.LPrintf(c.stmt.address + " is network crashed !!!!")
	atomic.StoreInt32(&c.nF, 1)
	atomic.StoreInt32(&configs.TestNF, 1)
}

// Recover the interface to recover from injected crash failure.
func (c *Manager) Recover() {
	configs.LPrintf(c.stmt.address + " is recovered !!!!")
	atomic.StoreInt32(&c.broken, 0)
	atomic.StoreInt32(&configs.TestCF, 0)
	c.stmt.queueLatch.Lock()
	for _, msg := range c.stmt.msgQueue {
		// on recovery, we pend the delayed messages for the current node.
		c.stmt.handleRequestType(msg)
	}
	c.stmt.msgQueue = c.stmt.msgQueue[:0]
	c.stmt.queueLatch.Unlock()
}

// NetRecover the interface to recover from injected network failure.
func (c *Manager) NetRecover() {
	configs.LPrintf(c.stmt.address + " is network recovered !!!!")
	atomic.StoreInt32(&c.nF, 0)
	atomic.StoreInt32(&configs.TestNF, 0)
}

func (c *Manager) isBroken() bool {
	return atomic.LoadInt32(&c.broken) == 1
}

func (c *Manager) isDisrupted() bool {
	return atomic.LoadInt32(&c.nF) == 1
}

func (c *Manager) GetStmt() *Context {
	return c.stmt
}
