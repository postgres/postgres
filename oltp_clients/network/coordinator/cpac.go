package coordinator

import (
	"FC/configs"
	"FC/network"
	"FC/utils"
	"strconv"
	"time"
)

// Here is a centralized version of PAC. We fix the leader to header node.
// PAC comes to this version if one node is always selected as the leader.

func (c *Manager) FTAgree4PAC(txn *TX, duration *time.Duration) bool {
	defer configs.TimeAdd(time.Now(), "3PC PreCommit", txn.TxnID, duration)
	handler := c.mustExistTxnHandler(txn.TxnID)
	c.logs.writeTxnState(txn, handler.State)
	handler.clearMsgPool()
	branches := make(map[string]*network.CoordinatorGossip)
	for _, v := range txn.Participants {
		branches[v] = network.NewTXPack(txn.TxnID, v, configs.PAC, txn.Participants)
	}
	//// build over, broadcast vote ////
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

// PACSubmit submit the transaction with C-PAC.
func (c *Manager) PACSubmit(tx *TX, info *utils.Info) bool {
	defer c.clearTxnHandler(tx.TxnID)
	if info == nil {
		info = utils.NewInfo(len(tx.Participants))
	}
	tx.Protocol = configs.ThreePC
	// since no leader is changed, no response would come with Decision = true or AcceptVal.
	// the LE + LD phase of PAC degenerate to the same as 3PC.
	ok := c.PreWrite(tx, &info.ST1, info) // Prepare message + write executions.
	if !ok {
		configs.DPrintf("Txn" + strconv.FormatUint(tx.TxnID, 10) + ": failed at pre-write")
		if !c.DecideBlock(tx, false, &info.ST2) {
			info.Failure = true
			return false
		} else {
			return false
		}
	} else {
		if !c.FTAgree4PAC(tx, &info.ST2) {
			info.Failure = true
			c.DecideBlock(tx, false, &info.ST2)
			return false
		} else {
			c.DecideAsync(tx, true, &info.ST3)
			return true
		}
	}
}
