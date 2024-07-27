package coordinator

import (
	"FC/configs"
	"FC/network"
	"FC/utils"
	"strconv"
	"time"
)

func (c *Manager) PreCommit(txn *TX, duration *time.Duration) bool {
	defer configs.TimeAdd(time.Now(), "3PC PreCommit", txn.TxnID, duration)
	handler := c.mustExistTxnHandler(txn.TxnID)
	c.logs.writeTxnState(txn, handler.State)
	handler.clearMsgPool()
	branches := make(map[string]*network.CoordinatorGossip)
	for _, v := range txn.Participants {
		branches[v] = network.NewTXPack(txn.TxnID, v, configs.ThreePC, txn.Participants)
	}
	for i, op := range branches {
		go txn.from.sendPreCommit(i, op)
	}
	select {
	case <-time.After(configs.CrashFailureTimeout):
		configs.TPrintf("acp finish after crash failure")
		return false
	case <-c.stmt.ctx.Done():
		configs.TPrintf("acp finish after ctx break")
		return false
	case <-handler.finish:
		// wait all branches to be committable.
		return handler.allACKCollected()
	}
}

// DecideAsync in 3PC after receiving all pre-commit ACKs, no need to wait for decide ACK
// since each participant has been committable with votes of all branches.
func (c *Manager) DecideAsync(txn *TX, isCommit bool, duration *time.Duration) {
	defer configs.TimeAdd(time.Now(), "Decide Async", txn.TxnID, duration)
	handler := c.ignoreIfNotExistTxnHandler(txn.TxnID)
	if handler == nil {
		return
	}
	//configs.Assert(handler.State == Committed, fmt.Sprintf("you should not 3pc decide without a commit decision TXN%v", txn.TxnID))
	c.logs.writeTxnState(txn, handler.State)
	handler.clearMsgPool()
	branches := make(map[string]*network.CoordinatorGossip)
	for _, v := range txn.Participants {
		branches[v] = network.NewTXPack(txn.TxnID, v, txn.Protocol, txn.Participants)
	}
	for i, op := range branches {
		go txn.from.sendDecide(i, op, isCommit)
	}
}

// ThreePCSubmit submit the transaction with 2PC.
func (c *Manager) ThreePCSubmit(tx *TX, info *utils.Info) bool {
	defer c.clearTxnHandler(tx.TxnID)
	if info == nil {
		info = utils.NewInfo(len(tx.Participants))
	}
	tx.Protocol = configs.ThreePC
	ok := c.PreWrite(tx, &info.ST1, info) // Prepare message + write executions.
	if !ok {
		// abort in phase one, follow 2PC abort path.
		configs.DPrintf("Txn" + strconv.FormatUint(tx.TxnID, 10) + ": failed at pre-write")
		if !c.DecideBlock(tx, ok, &info.ST2) {
			info.Failure = true
			return false
		} else {
			return ok
		}
	} else {
		if !c.PreCommit(tx, &info.ST2) {
			info.Failure = true
			c.DecideBlock(tx, false, &info.ST3) // failure has been asserted, ignore the return
			return false
		} else {
			c.DecideAsync(tx, true, &info.ST3)
			return true
		}
	}
}
