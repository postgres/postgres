package coordinator

import (
	"FC/configs"
	"FC/network"
	"FC/utils"
	"fmt"
	"time"
)

// DecideTransitBeforeDecide in EC.
func (c *Manager) DecideTransitBeforeDecide(txn *TX, isCommit bool, duration *time.Duration) {
	defer configs.TimeAdd(time.Now(), "Decide EC", txn.TxnID, duration)
	handler := c.mustExistTxnHandler(txn.TxnID)
	if txn.Protocol == configs.FCcf {
		txn.Protocol = configs.EasyCommit
		// in fc-cf, the decision phase is the same as easy commit.
		configs.Assert(isCommit, "fc-cf should not transit-before-decide abort")
		handler.State = TransitCommit
	}
	configs.Assert(handler.State == TransitCommit || handler.State == TransitAbort,
		fmt.Sprintf("you should not ec decide without entering TrA or TrC State TXN%v", txn.TxnID))
	c.logs.writeTxnState(txn, handler.State) // log transit-commit/abort
	handler.clearMsgPool()
	branches := make(map[string]*network.CoordinatorGossip)
	for _, v := range txn.Participants {
		branches[v] = network.NewTXPack(txn.TxnID, v, txn.Protocol, txn.Participants)
	}
	for i, op := range branches {
		txn.from.sendDecide(i, op, isCommit)
	}
	// after sending all messages, change State and log final decision.
	if isCommit {
		handler.transit(TransitCommit, Committed)
	} else {
		handler.transit(TransitAbort, Aborted)
	}
	c.logs.writeTxnState(txn, handler.State)
}

// EasySubmit submit the transaction with EC.
func (c *Manager) EasySubmit(tx *TX, info *utils.Info) bool {
	defer c.clearTxnHandler(tx.TxnID)
	if info == nil {
		info = utils.NewInfo(len(tx.Participants))
	}
	tx.Protocol = configs.EasyCommit
	ok := c.PreWrite(tx, &info.ST1, info) // Prepare message + write executions.
	if info.Failure {
		// when crash failure happens, should not call TrDecide.
		// change back mark to 2PC to enable commit ack.
		tx.Protocol = configs.TwoPC
		c.DecideBlock(tx, false, &info.ST2)
	} else {
		c.DecideTransitBeforeDecide(tx, ok, &info.ST2) // easy commit transit-before decide.
	}
	return ok
}
