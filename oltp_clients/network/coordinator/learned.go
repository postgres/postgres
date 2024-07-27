package coordinator

import (
	"FC/configs"
	"FC/network"
	"FC/network/learner"
	"FC/storage"
	"FC/utils"
	"fmt"
	"strconv"
	"time"
)

// PreWriteSubset prepare phase for some the shards (stores).
func (c *Manager) PreWriteSubset(txn *TX, aims []string, stage int, delays map[string]time.Duration, duration *time.Duration, info *utils.Info) bool {
	defer configs.TimeAdd(time.Now(), fmt.Sprintf("Stage %v PreWrite for %v", stage, aims), txn.TxnID, duration)
	handler := c.createIfNotExistTxnHandler(txn.TxnID, txn.Protocol, len(txn.Participants))
	if stage == 0 {
		handler.transit(None, PreWrite)
	} else {
		configs.Assert(handler.State == PreWrite,
			fmt.Sprintf("the transaction should have entered the pre-write state, but in %v", handler.State))
	}
	c.logs.writeTxnState(txn, handler.State)
	branches := make(map[string]*network.CoordinatorGossip)
	for _, v := range aims {
		branches[v] = network.NewTXPack(txn.TxnID, v, txn.Protocol, txn.Participants)
	}
	for _, v := range txn.OptList {
		br, ok := branches[v.Shard]
		if !ok { // skip
			continue
		}
		switch v.Type {
		case storage.UpdateOpt:
			br.AppendUpdate(v.Table, v.Shard, v.Key, v.Value)
		case storage.ReadOpt:
			br.AppendRead(v.Table, v.Shard, v.Key)
		default:
			panic("invalid operation")
		}
	}
	handler.VoterNumber = len(branches)
	if handler.VoterNumber != len(aims) {
		panic("incorrect voter number")
	}
	handler.clearMsgPool()
	for i, op := range branches {
		go func(aim string, delay time.Duration, txnBr *network.CoordinatorGossip) {
			time.Sleep(delay)
			txn.from.sendPreWrite(aim, txnBr)
		}(i, delays[i], op)
	}
	timeout := configs.CrashFailureTimeout
	if configs.EnableQuickPreWriteAbort {
		timeout = c.stmt.GetNetworkTimeOut(txn.Participants, configs.PreWriteACK)
	}
	select {
	case <-time.After(timeout):
		configs.TPrintf("TXN"+strconv.FormatUint(txn.TxnID, 10)+": acp finish after crash failure, %v ACK collected", handler.MsgCount)
		handler.State = Aborted // thread-unsafe
		info.Failure = true
		return false
	case <-c.stmt.ctx.Done():
		configs.TPrintf("TXN" + strconv.FormatUint(txn.TxnID, 10) + ": acp finish after ctx break")
		handler.State = Aborted // thread-unsafe
		info.Failure = true
		return false
	case <-handler.finish:
		return handler.canCommitWithAllVotes()
	}
}

// LearnedSubmit submit the transaction with two speed levels.
func (c *Manager) LearnedSubmit(tx *TX, info *utils.Info) bool {
	defer c.clearTxnHandler(tx.TxnID)
	if info == nil {
		info = utils.NewInfo(len(tx.Participants))
	}
	tx.Protocol = configs.LearnedC // the participant-side execution of level commit is the same as 2PC
	stages := learner.GetParticipantDivide(tx.Participants)
	if stages == nil {
		// current condition is too crowded, directly abort the transaction to avoid extra cost.
		return false
	}
	delays := learner.GetDelayVector(tx.Participants)
	//configs.JPrint(stages)
	for i, nodes := range stages {
		ok := c.PreWriteSubset(tx, nodes, i, delays, &info.ST1, info)
		if !ok {
			configs.DPrintf("Txn"+strconv.FormatUint(tx.TxnID, 10)+
				": failed at pre-write stage %v, including %v", i, nodes)
			c.DecideAsync(tx, false, &info.ST2)
			return false
		}
	}
	if !c.DecideBlock(tx, true, &info.ST2) {
		info.Failure = true
		// failure case, participants recover by reading logs.
		return false
	} else {
		return true
	}
}
