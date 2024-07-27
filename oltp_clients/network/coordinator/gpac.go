package coordinator

import (
	"FC/configs"
	"FC/network"
	"FC/storage"
	"FC/utils"
	"fmt"
	"strconv"
	"time"
)

// Here is G-PAC. We fix the leader to the coordinator.
// Here is a generalized + replicated version of PAC. We fix the leader to the coordinator node.

func extractAimAddress4Branch(branchName string) string {
	i := 0
	for branchName[i] != '-' {
		i++
	}
	//configs.JPrint(branchName[i+1:])
	return branchName[i+1:]
}

// PreWriteSuperSet pre-write phase of G-PAC to retrieve votes from Participants.
func (c *Manager) PreWriteSuperSet(txn *TX, duration *time.Duration, info *utils.Info) bool {
	defer configs.TimeAdd(time.Now(), "G-PAC PreWrite", txn.TxnID, duration)
	handler := c.createIfNotExistTxnHandler(txn.TxnID, txn.Protocol, len(txn.Participants))
	handler.transit(None, PreWrite)
	c.logs.writeTxnState(txn, handler.State)
	branches := make(map[string]*network.CoordinatorGossip)
	for _, v := range txn.Participants {
		//configs.JPrint(txn.from.Replicas)
		configs.Assert(len(txn.from.Replicas[v]) == configs.NumberOfReplicas, "not enough replicas")
		for _, r := range txn.from.Replicas[v] {
			branches[v+"-"+r] = network.NewReplicatedTXPack(txn.TxnID, v, r, configs.GPAC, txn.Participants)
		}
	}
	for _, v := range txn.OptList {
		switch v.Type {
		case storage.UpdateOpt:
			configs.Assert(len(txn.from.Replicas[v.Shard]) == configs.NumberOfReplicas, "not enough replicas")
			for _, r := range txn.from.Replicas[v.Shard] { // get vote from all replicas of a shard.
				branches[v.Shard+"-"+r].AppendUpdate(v.Table, v.Shard, v.Key, v.Value)
			}
		case storage.ReadOpt:
			configs.Assert(len(txn.from.Replicas[v.Shard]) == configs.NumberOfReplicas, "not enough replicas")
			for _, r := range txn.from.Replicas[v.Shard] { // get vote from all replicas of a shard.
				branches[v.Shard+"-"+r].AppendRead(v.Table, v.Shard, v.Key)
			}
		default:
			panic("invalid operation")
		}
	}
	handler.VoterNumber = len(txn.Participants)
	handler.clearMsgPool()
	//// build over, broadcast vote ////
	for i, op := range branches {
		go txn.from.sendPreWrite(extractAimAddress4Branch(i), op)
	}
	select {
	case <-time.After(configs.CrashFailureTimeout):
		configs.TPrintf("acp finish after crash failure, write set")
		panic("crash failure here")
		handler.State = Aborted // thread-unsafe
		info.Failure = true
		return false
	case <-c.stmt.ctx.Done():
		configs.TPrintf("acp finish after ctx break")
		handler.State = Aborted // thread-unsafe
		info.Failure = true
		return false
	case <-handler.finish:
		return handler.ftPreWrite()
	}
}

// FTAgree4GPAC get agreement from a super-set.
func (c *Manager) FTAgree4GPAC(txn *TX, duration *time.Duration, info *utils.Info) bool {
	defer configs.TimeAdd(time.Now(), "G-PAC PreCommit", txn.TxnID, duration)
	handler := c.mustExistTxnHandler(txn.TxnID)
	c.logs.writeTxnState(txn, handler.State)
	handler.clearMsgPool()
	branches := make(map[string]*network.CoordinatorGossip)
	for _, v := range txn.Participants {
		configs.Assert(len(txn.from.Replicas[v]) == configs.NumberOfReplicas, "not enough replicas")
		for _, r := range txn.from.Replicas[v] {
			branches[v+"-"+r] = network.NewReplicatedTXPack(txn.TxnID, v, r, configs.GPAC, txn.Participants)
		}
	}
	for i, op := range branches {
		go txn.from.sendPreCommit(extractAimAddress4Branch(i), op)
	}
	select {
	case <-time.After(configs.CrashFailureTimeout):
		configs.TPrintf("TXN%v:acp finish after crash failure FT-Agree", txn.TxnID)
		configs.JPrint(handler)
		panic(fmt.Sprintf("TXN%v: crash failure here", txn.TxnID))
		handler.State = Aborted // thread-unsafe
		info.Failure = true
		return false
	case <-c.stmt.ctx.Done():
		configs.TPrintf("acp finish after ctx break")
		handler.State = Aborted // thread-unsafe
		info.Failure = true
		return false
	case <-handler.finish:
		return handler.quorumACKCollected()
	}
}

func (c *Manager) DecideReplicated(txn *TX, isCommit bool, duration *time.Duration) bool {
	defer configs.TimeAdd(time.Now(), "Decide Block", txn.TxnID, duration)
	handler := c.mustExistTxnHandler(txn.TxnID)
	c.logs.writeTxnState(txn, handler.State)
	handler.clearMsgPool()

	//if isCommit {
	//	handler.State = Committed
	//} else {
	//	handler.State = Aborted
	//}
	//handler.State = Committed
	branches := make(map[string]*network.CoordinatorGossip)
	for _, v := range txn.Participants {
		configs.Assert(len(txn.from.Replicas[v]) == configs.NumberOfReplicas, "not enough replicas")
		for _, r := range txn.from.Replicas[v] {
			branches[v+"-"+r] = network.NewReplicatedTXPack(txn.TxnID, v, r, configs.GPAC, txn.Participants)
		}
	}
	for i, op := range branches {
		txn.from.sendDecide(extractAimAddress4Branch(i), op, isCommit)
	}
	select {
	case <-time.After(configs.CrashFailureTimeout):
		//panic("crash failure here")
		configs.TPrintf("acp finish after crash failure in decide replicated")
		return false
	case <-c.stmt.ctx.Done():
		configs.TPrintf("acp finish after ctx break")
		return false
	case <-handler.finish:
		return handler.allACKCollected()
	}
}

func (c *Manager) DecideReplicatedAsync(txn *TX, isCommit bool, duration *time.Duration) {
	defer configs.TimeAdd(time.Now(), "Decide Block", txn.TxnID, duration)
	handler := c.mustExistTxnHandler(txn.TxnID)
	c.logs.writeTxnState(txn, handler.State)
	handler.clearMsgPool()
	branches := make(map[string]*network.CoordinatorGossip)
	for _, v := range txn.Participants {
		configs.Assert(len(txn.from.Replicas[v]) == configs.NumberOfReplicas, "not enough replicas")
		for _, r := range txn.from.Replicas[v] {
			branches[v+"-"+r] = network.NewReplicatedTXPack(txn.TxnID, v, r, configs.GPAC, txn.Participants)
		}
	}
	for i, op := range branches {
		txn.from.sendDecide(extractAimAddress4Branch(i), op, isCommit)
	}
}

// GPACSubmit submit the transaction with G-PAC.
func (c *Manager) GPACSubmit(tx *TX, info *utils.Info) bool {
	defer c.clearTxnHandler(tx.TxnID)
	if info == nil {
		info = utils.NewInfo(len(tx.Participants))
	}
	tx.Protocol = configs.GPAC
	// since no leader is changed, no response would come with Decision = true or AcceptVal.
	// the LE + LD phase of PAC degenerate to the same as 3PC.
	ok := c.PreWriteSuperSet(tx, &info.ST1, info) // non-blocking 2 delays.
	if !ok {
		configs.DPrintf("Txn" + strconv.FormatUint(tx.TxnID, 10) + ": failed at pre-write")
		ok = c.DecideReplicated(tx, false, &info.ST2)
		if !ok {
			info.Failure = true
		}
		return false
	}
	//ok = c.DecideReplicated(tx, ok, &info.ST2)
	//return ok

	ok = c.FTAgree4GPAC(tx, &info.ST2, info)
	if !ok {
		configs.DPrintf("Txn" + strconv.FormatUint(tx.TxnID, 10) + ": failed at agree")
		ok = c.DecideReplicated(tx, false, &info.ST2)
		if !ok {
			info.Failure = true
		}
		return false
	} else {
		// As mentioned in Paper, the final commit decision is sent asynchronously.
		c.DecideReplicatedAsync(tx, true, &info.ST3)
		return true
	}
}
