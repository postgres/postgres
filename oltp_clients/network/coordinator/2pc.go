package coordinator

import (
	"FC/configs"
	"FC/network"
	"FC/storage"
	"FC/utils"
	"strconv"
	"time"
)

// DecideBlock sends the decision and require all ACKs to end.
func (c *Manager) DecideBlock(txn *TX, isCommit bool, duration *time.Duration) bool {
	defer configs.TimeAdd(time.Now(), "Decide Block", txn.TxnID, duration)
	handler := c.mustExistTxnHandler(txn.TxnID)
	if isCommit && txn.Protocol == configs.LearnedC {
		handler.transit(PreWrite, Committed)
	}
	//configs.Assert(handler.State == Committed || handler.State == Aborted, fmt.Sprintf("you should not decide without a decision TXN%v", txn.TxnID))
	c.logs.writeTxnState(txn, handler.State)
	handler.clearMsgPool()
	branches := make(map[string]*network.CoordinatorGossip)
	for _, v := range txn.Participants {
		branches[v] = network.NewTXPack(txn.TxnID, v, txn.Protocol, txn.Participants)
	}
	for i, op := range branches {
		go txn.from.sendDecide(i, op, isCommit)
	}
	select {
	case <-time.After(configs.CrashFailureTimeout):
		configs.TPrintf("TXN"+strconv.FormatUint(txn.TxnID, 10)+": acp finish after crash failure, %v ACK collected", handler.MsgCount)
		return false
	case <-c.stmt.ctx.Done():
		configs.TPrintf("TXN" + strconv.FormatUint(txn.TxnID, 10) + ": acp finish after ctx break")
		return false
	case <-handler.finish:
		return handler.allACKCollected()
	}
}

// PreWrite prepare phase + write operation executions.
func (c *Manager) PreWrite(txn *TX, duration *time.Duration, info *utils.Info) bool {
	defer configs.TimeAdd(time.Now(), "3PC/2PC PreWrite", txn.TxnID, duration)
	handler := c.createIfNotExistTxnHandler(txn.TxnID, txn.Protocol, len(txn.Participants))
	handler.transit(None, PreWrite)
	c.logs.writeTxnState(txn, handler.State)
	branches := make(map[string]*network.CoordinatorGossip)
	for _, v := range txn.Participants {
		branches[v] = network.NewTXPack(txn.TxnID, v, txn.Protocol, txn.Participants)
	}
	for _, v := range txn.OptList {
		switch v.Type {
		case storage.UpdateOpt:
			branches[v.Shard].AppendUpdate(v.Table, v.Shard, v.Key, v.Value)
		case storage.ReadOpt:
			branches[v.Shard].AppendRead(v.Table, v.Shard, v.Key)
		default:
			panic("invalid operation")
		}
	}
	handler.VoterNumber = len(branches)
	if handler.VoterNumber != len(txn.Participants) {
		panic("incorrect voter number")
	}
	handler.clearMsgPool()
	for i, op := range branches {
		go txn.from.sendPreWrite(i, op)
	}
	timeout := configs.CrashFailureTimeout
	if configs.EnableQuickPreWriteAbort {
		// optimization regarding crash failure happening: if crash failure happens the Protocol shall not be kept waiting.
		timeout = c.stmt.GetNetworkTimeOut(txn.Participants, configs.PreWriteACK)
		//configs.JPrint("timeout = " + timeout.String())
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

// TwoPCSubmit submit the transaction with 2PC.
func (c *Manager) TwoPCSubmit(tx *TX, info *utils.Info) bool {
	defer c.clearTxnHandler(tx.TxnID)
	if info == nil {
		info = utils.NewInfo(len(tx.Participants))
	}
	tx.Protocol = configs.TwoPC
	ok := c.PreWrite(tx, &info.ST1, info) // Prepare message + write executions.
	if !ok {
		configs.DPrintf("Txn" + strconv.FormatUint(tx.TxnID, 10) + ": failed at pre-write")
		c.DecideAsync(tx, false, &info.ST2)
		return false
	} else {
		if !c.DecideBlock(tx, true, &info.ST2) {
			info.Failure = true
			// failure case, participants recover by reading logs.
			return false
		} else {
			return true
		}
	}
}
