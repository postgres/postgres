package participant

import (
	"FC/configs"
	"FC/network"
	"FC/network/detector"
	"FC/storage"
	"fmt"
	"strconv"
	"time"
)

const (
	No  = 0
	Yes = 1
)

// TXNBranch is used to handle one specific transaction.
type TXNBranch struct {
	Kv           *storage.Shard
	Res          *detector.KvRes
	from         *Manager
	OptList      []storage.TXOpt
	voteReceived []bool
	Vote         bool
	TID          uint64
	beginTime    time.Time
	info         *Info
}

func NewParticipantBranch(msg *network.CoordinatorGossip, kv *storage.Shard, manager *Manager) *TXNBranch {
	res := &TXNBranch{
		TID:          msg.TxnID,
		Kv:           kv,
		Res:          detector.NewKvRes(int(msg.TxnID), kv.GetID()),
		from:         manager,
		OptList:      msg.OptList,
		Vote:         false,
		voteReceived: make([]bool, 0),
		beginTime:    time.Time{},
		info:         NewInfo(msg.Protocol, msg.TxnID),
	}
	res.pruneRepeatedOperations()
	return res
}

func (c *TXNBranch) PreCommit() bool {
	if configs.EnableReplication {
		return c.Kv.PreCommitAsync(uint32(c.TID))
	} else {
		c.Kv.PreCommit(uint32(c.TID))
		return true
	}
}

// pruneRepeatedOperations Disabled to avoid affecting performance evaluation.
func (c *TXNBranch) pruneRepeatedOperations() {
	// Deadlock prevention as suggested in https://www.postgresql.org/docs/current/explicit-locking.html#LOCKING-DEADLOCKS
	// Rule 1: the key shall be granted in the same order.
	// Rule 2: first read, then write.
	N := len(c.OptList)
	for i := 0; i < N-1; i++ {
		for j := i + 1; j < N; j++ {
			if c.OptList[i].Key > c.OptList[j].Key ||
				(c.OptList[i].Key == c.OptList[j].Key && c.OptList[i].Type > c.OptList[j].Type) {
				c.OptList[i], c.OptList[j] = c.OptList[j], c.OptList[i]
			}
		}
	}

	//tempOptList := make([]storage.TXOpt, 0)
	//vis := make(map[string]bool)
	//for i := len(c.OptList) - 1; i >= 0; i-- {
	//	// keep the last write operation for each key
	//	op := c.OptList[i]
	//	if op.Type == storage.ReadOpt {
	//		continue
	//	}
	//	s := op.Shard + ":" + op.Table + ":" + strconv.Itoa(int(op.Key)) + ":" + strconv.FormatUint(uint64(op.Type), 10)
	//	if !vis[s] {
	//		vis[s] = true
	//		tempOptList = append(tempOptList, op)
	//	}
	//}
	//for i, ln := 0, len(c.OptList); i < ln; i++ {
	//	// keep the first read operation for each key
	//	op := c.OptList[i]
	//	if op.Type == storage.UpdateOpt {
	//		continue
	//	}
	//	s := op.Shard + ":" + op.Table + ":" + strconv.Itoa(int(op.Key)) + ":" + strconv.FormatUint(uint64(op.Type), 10)
	//	if !vis[s] {
	//		vis[s] = true
	//		tempOptList = append(tempOptList, op)
	//	}
	//}
	//for i, ln := 0, len(tempOptList); i < ln/2; i++ {
	//	j := ln - i - 1
	//	tempOptList[i], tempOptList[j] = tempOptList[j], tempOptList[i]
	//}
	//c.OptList = tempOptList
}

// ProposeFCFF handles the Propose Phase of FCff-FF, safe and does not break atomicity.
// This is faster but could suffer from crash failure: single node crash could block the whole system.
// probabilistic analysis reveals that it shall always be faster than 2PC.
func (c *TXNBranch) ProposeFCFF(msg *network.CoordinatorGossip, sent time.Time) *detector.KvRes {
	if !c.GetVote() {
		// if vote to abort, it will abort regardless of the ACP used.
		configs.TPrintf("TXN" + strconv.FormatUint(c.TID, 10) + ": " + c.from.stmt.address + " abort for PreWrite")
		c.Res.SetSelfResult(false, false, true)
		c.from.broadCastVote(msg, No, msg.ShardID, msg.ParticipantAddresses, sent)
		configs.Assert(c.from.Abort(msg), "impossible case, abort should not fail with all locks")
	} else {
		configs.DPrintf("TXN" + strconv.FormatUint(msg.TxnID, 10) + ": " + "Yes Voting From " + c.from.stmt.address)
		hd := c.from.createIfNotExistMsgPool(msg.ShardID, msg.TxnID, len(msg.ParticipantAddresses))
		rem := c.from.stmt.GetNetworkTimeOut(msg.ParticipantAddresses)
		configs.LPrintf("TXN" + strconv.FormatUint(msg.TxnID, 10) + ": " + " participant side timeout = " + rem.String())
		c.from.broadCastVote(msg, Yes, msg.ShardID, msg.ParticipantAddresses, sent)
		select {
		case <-time.After(rem):
			configs.TPrintf("TXN" + strconv.FormatUint(c.TID, 10) + ": " + c.from.stmt.address + " auto commit")
			c.Res.SetSelfResult(true, true, false)
			return c.Res
		case <-c.from.stmt.ctx.Done():
			configs.TPrintf("TXN" + strconv.FormatUint(c.TID, 10) + ": " + c.from.stmt.address + " abort for ctx done")
			c.Res.SetSelfResult(true, false, false)
			return c.Res
		case <-hd.finish:
			if hd.canAbort {
				c.Abort()
				c.Res.SetSelfResult(true, false, true)
				return c.Res
			} else {
				c.Commit()
				c.Res.SetSelfResult(true, true, true)
				return c.Res
			}
		}
	}
	return c.Res
}

// ProposeFCCF handles the Propose Phase of FCff-CF, safe and does not break atomicity.
// This is faster but could suffer from network delay: network delay could abort the whole system.
func (c *TXNBranch) ProposeFCCF(msg *network.CoordinatorGossip, sent time.Time) *detector.KvRes {
	if !c.GetVote() {
		// if vote to abort, it will abort regardless of the ACP used.
		configs.TPrintf("TXN" + strconv.FormatUint(c.TID, 10) + ": " + c.from.stmt.address + " abort for PreWrite")
		c.Res.SetSelfResult(false, false, true)
		c.from.broadCastVote(msg, No, msg.ShardID, msg.ParticipantAddresses, sent)
		configs.Assert(c.from.Abort(msg), "impossible case, abort should not fail with all locks")
	} else {
		configs.DPrintf("TXN" + strconv.FormatUint(msg.TxnID, 10) + ": " + "Yes Voting From " + c.from.stmt.address)
		hd := c.from.createIfNotExistMsgPool(msg.ShardID, msg.TxnID, len(msg.ParticipantAddresses))
		rem := c.from.stmt.GetNetworkTimeOut(msg.ParticipantAddresses)
		configs.LPrintf("TXN" + strconv.FormatUint(msg.TxnID, 10) + ": " + " participant side timeout = " + rem.String())
		c.from.broadCastVote(msg, Yes, msg.ShardID, msg.ParticipantAddresses, sent)
		select {
		case <-time.After(rem):
			// abort directly after the timeout window.
			configs.TPrintf("TXN" + strconv.FormatUint(c.TID, 10) + ": " + c.from.stmt.address + " auto commit")
			c.Res.SetSelfResult(true, false, true)
			configs.Assert(c.from.Abort(msg), "impossible case, abort should not fail with all locks")
			return c.Res
		case <-c.from.stmt.ctx.Done():
			configs.TPrintf("TXN" + strconv.FormatUint(c.TID, 10) + ": " + c.from.stmt.address + " abort for ctx done")
			c.Res.SetSelfResult(true, false, true)
			configs.Assert(c.from.Abort(msg), "impossible case, abort should not fail with all locks")
			return c.Res
		case <-hd.finish:
			if hd.canAbort {
				c.Abort()
				c.Res.SetSelfResult(true, false, true)
				return c.Res
			} else {
				c.Res.SetSelfResult(true, true, false)
				return c.Res
			}
		}
	}
	return c.Res
}

// ProposeFCFFNotSafe pre-commit a transaction with failure-free level, could break atomicity.
// abort gets directly finished, but the commit shall be performed in epoch to guarantee safety.
func (c *TXNBranch) ProposeFCFFNotSafe(msg *network.CoordinatorGossip, sent time.Time) *detector.KvRes {
	if !c.GetVote() {
		// if vote to abort, it will abort regardless of the ACP used.
		configs.TPrintf("TXN" + strconv.FormatUint(c.TID, 10) + ": " + c.from.stmt.address + " abort for PreWrite")
		c.Res.SetSelfResult(false, false, true)
		c.from.broadCastVote(msg, No, msg.ShardID, msg.ParticipantAddresses, sent)
		configs.Assert(c.from.Abort(msg), "impossible case, abort should not fail with all locks")
	} else {
		configs.DPrintf("TXN" + strconv.FormatUint(msg.TxnID, 10) + ": " + "failure-free level  " + c.from.stmt.address)
		hd := c.from.createIfNotExistMsgPool(msg.ShardID, msg.TxnID, len(msg.ParticipantAddresses))
		rem := c.from.stmt.GetNetworkTimeOut(msg.ParticipantAddresses) - time.Since(sent)
		configs.DPrintf("TXN" + strconv.FormatUint(msg.TxnID, 10) + ": " + " participant side timeout = " + rem.String())
		select {
		case <-time.After(rem): // No vote not received within the timeout window, thus directly pre-commit the batch.
			configs.TPrintf("TXN" + strconv.FormatUint(c.TID, 10) + ": " + c.from.stmt.address + " auto commit")
			c.CommitInEpoch(msg.EpochNum)
			c.Res.SetSelfResult(true, true, false)
			return c.Res
		case <-c.from.stmt.ctx.Done():
			configs.TPrintf("TXN" + strconv.FormatUint(c.TID, 10) + ": " + c.from.stmt.address + " abort for ctx done")
			c.Res.SetSelfResult(true, false, false)
			c.Abort()
			return c.Res
		case <-hd.finish:
			// No vote received, abort the transaction.
			c.Abort()
			c.Res.SetSelfResult(true, false, true)
			return c.Res
		}
	}
	return c.Res
}

// PreWrite execute write operations and persist redo logs.
func (c *TXNBranch) PreWrite() bool {
	defer configs.TimeTrack(time.Now(), "PreWrite", c.TID)
	c.Vote = c.GetVote()
	return c.Vote
}

func (c *TXNBranch) PreRead() (map[string]*storage.RowData, bool) {
	result := make(map[string]*storage.RowData)
	for _, v := range c.OptList {
		switch v.Type {
		case storage.ReadOpt:
			val, ok := c.Kv.ReadTxn(v.Table, uint32(c.TID), v.Key)
			if !ok {
				return nil, false
			} else {
				result[v.GetKey()] = val
			}
		default:
			panic(fmt.Errorf("no update operation shall be passed during pre-read"))
		}
	}
	return result, true
}

func (c *TXNBranch) CommitInEpoch(epochNum uint64) bool {
	defer configs.TimeTrack(time.Now(), "Commit", c.TID)
	return c.Kv.Commit(uint32(c.TID))
}

func (c *TXNBranch) AbortInEpoch(epochNum uint64) bool {
	defer configs.TimeTrack(time.Now(), "Commit", c.TID)
	return c.Kv.RollBack(uint32(c.TID))
}

//
//func (c *TXNBranch) PersistEpoch(epochNum uint64) bool {
//	defer configs.TimeTrack(time.Now(), "Commit", c.TID)
//	return c.Kv.Commit(uint32(c.TID))
//}

func (c *TXNBranch) Commit() bool {
	//if c.info.Latency == 0 && !c.beginTime.IsZero() {
	//	c.info.Latency = time.Since(c.beginTime)
	//}
	// Such decide time statistic is not concurrently safe regarding atomic commit protocols such as the EasyCommit and FC.
	defer configs.TimeAdd(time.Now(), "Commit", c.TID, &c.info.DecideTime)
	defer configs.TimeLoad(c.beginTime, "LCC", c.TID, &c.info.Latency)
	return c.Kv.Commit(uint32(c.TID))
}

func (c *TXNBranch) Abort() bool {
	//if c.info.Latency == 0 && !c.beginTime.IsZero() {
	//	c.info.Latency = time.Since(c.beginTime)
	//}
	if c.beginTime.IsZero() {
		// the transaction branch could get aborted before even execution.
		c.beginTime = time.Now()
	}
	// Such decide time statistic is not concurrently safe regarding atomic commit protocols such as the EasyCommit and FC.
	defer configs.TimeAdd(time.Now(), "Abort", c.TID, &c.info.DecideTime)
	defer configs.TimeLoad(c.beginTime, "LCC", c.TID, &c.info.Latency)
	return c.Kv.RollBack(uint32(c.TID))
}

// GetVote checks if we can get all the locks to ensure ACID for write operations.
// tx should only contain write operations.
// This can only be called once.
func (c *TXNBranch) GetVote() bool {
	if c.beginTime.IsZero() {
		c.beginTime = time.Now()
	}
	for _, v := range c.OptList {
		if v.Type == storage.UpdateOpt {
			var ok = false
			if configs.NoBlindWrite && !configs.StoredProcedure {
				// in case of interactive mode, the db does not know if the read lock will upgrade to write lock latter.
				_, ok = c.Kv.ReadTxn(v.Table, uint32(c.TID), v.Key)
				if !ok {
					c.info.ExecutionTime += time.Since(c.beginTime)
					return false
				}
			}
			ok = c.Kv.UpdateTxn(v.Table, uint32(c.TID), v.Key, v.Value)
			if !ok {
				c.info.ExecutionTime += time.Since(c.beginTime)
				return false
			}
		} else {
			if !configs.StoredProcedure {
				continue // skip the read operations regarding the pre-read + getvote.
			}
			if v.Type == storage.ReadOpt {
				_, ok := c.Kv.ReadTxn(v.Table, uint32(c.TID), v.Key)
				if !ok {
					c.info.ExecutionTime += time.Since(c.beginTime)
					return false
				}
			} else {
				panic(fmt.Errorf("invalid operation"))
			}
		}
	}
	c.info.ExecutionTime += time.Since(c.beginTime)
	if c.info.ACP == configs.NoACP {
		time.Sleep(c.Kv.LockWindowInjectedDelay)
		return true
	} else {
		return c.Prepare()
	}
}

func (c *TXNBranch) Prepare() bool {
	defer configs.TimeAdd(time.Now(), "Prepare", c.TID, &c.info.PrepareTime)
	return c.Kv.Prepare(uint32(c.TID))
}
