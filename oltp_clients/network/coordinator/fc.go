package coordinator

import (
	"FC/configs"
	"FC/network"
	"FC/network/detector"
	"FC/storage"
	"FC/utils"
	"fmt"
	"time"
)

func (c *Manager) FCPropose(txn *TX, duration *time.Duration, info *utils.Info) *detector.KvResult {
	defer configs.TimeAdd(time.Now(), "FCFF Propose", txn.TxnID, duration)
	handler := c.createIfNotExistTxnHandler(txn.TxnID, txn.Protocol, len(txn.Participants))
	handler.State = None
	handler.transit(None, Propose)
	//c.logs.writeTxnState(txn, handler.State)
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
		go txn.from.sendPropose(i, op)
	}
	timeout := configs.CrashFailureTimeout
	if txn.Protocol == configs.FCcf {
		timeout = c.stmt.GetNetworkTimeOut(txn.Participants, configs.FCResults)
	}
	select {
	case <-time.After(timeout):
		configs.TPrintf("acp finish after crash failure")
		handler.State = Aborted // thread-unsafe
		info.Failure = true
		info.Result = detector.NewKvResult(handler.VoterNumber)
		//handler.transit(PreWrite, Aborted)
		return nil
	case <-c.stmt.ctx.Done():
		configs.TPrintf("acp finish after ctx break")
		handler.State = Aborted // thread-unsafe
		info.Failure = true
		info.Result = detector.NewKvResult(handler.VoterNumber)
		//handler.transit(PreWrite, Aborted)
		return nil
	case <-handler.finish:
		info.Result = handler.extractFCResult()
		return info.Result
	}
}

// FCSubmit submit the transaction with FC.
func (c *Manager) FCSubmit(tx *TX, info *utils.Info) bool {
	if info == nil {
		info = utils.NewInfo(len(tx.Participants))
	}
	defer func() {
		if info.IsCommit {
			detector.Add_th()
		}
	}()
	level, ts := c.Lsm.Start(tx.Participants)
	info.Result = nil
	info.Level = int(level)
	if level == detector.CFNF {
		info.IsCommit = c.EasySubmit(tx, info)
	} else {
		if level == detector.NoCFNoNF {
			info.IsCommit = c.FCffSubmit(tx, info, ts)
		} else if level == detector.CFNoNF {
			info.IsCommit = c.FCcfSubmit(tx, info)
			err := c.Lsm.Finish(tx.Participants, info.Result, level, ts)
			if err != nil {
				panic(err)
			}
		} else {
			panic("invalid Protocol level")
		}
	}
	return info.IsCommit
}

func (c *Manager) performFCProposeTask(txn *TX, duration *time.Duration, info *utils.Info, result chan<- *detector.KvResult) {
	result <- c.FCPropose(txn, duration, info)
}

// FCffSubmit submit the transaction with FC-FF.
func (c *Manager) FCffSubmit(tx *TX, info *utils.Info, ts int) bool {
	defer c.clearTxnHandler(tx.TxnID)
	if info == nil {
		info = utils.NewInfo(len(tx.Participants))
	}
	tx.Protocol = configs.FCff
	// Try to propose, and then get the results for failure analysis.
	// The catch of failure should finish before the transaction return. The catch of failure shall be addressed by the
	// channel functions.
	resChan := make(chan *detector.KvResult)
	go c.performFCProposeTask(tx, &info.ST1, info, resChan)
	var res *detector.KvResult
	delayTimeoutWindow := c.stmt.GetNetworkTimeOut(tx.Participants, configs.FCResults) / 2 * 3
	//configs.JPrint(delayTimeoutWindow.String())
	select {
	case res = <-resChan:
	case <-time.After(delayTimeoutWindow):
		// rise the failure before the transaction finishes.
		handler := c.ignoreIfNotExistTxnHandler(tx.TxnID)
		if handler != nil {
			res = handler.extractFCResult()
		}
	}
	if res == nil {
		// this only happens when there is a crash failure.
		info.Failure = true
		c.DecideAsync(tx, false, &info.ST2)
		// wait for another node to decide abort/commit.
		return false
	}
	corr := res.Correct(detector.NoCFNoNF)
	if !corr {
		info.IncorrectAssumption = true
	}
	info.Result = res
	err := c.Lsm.Finish(tx.Participants, info.Result, detector.NoCFNoNF, ts)
	if err != nil {
		panic(err)
	}
	var ok bool
	if res.Decision != detector.UnDecided {
		ok = res.Decision == detector.Commit
		if !res.AllPersisted() { // some are tentative, sync decision for them.
			c.DecideAsync(tx, ok, &info.ST2)
		}
		return ok
	} else {
		// undecided could happen when a node crashes before sending vote
		if res.AppendFinished() {
			// all of them voted yes, since no abort decision received
			ok = true
			c.DecideAsync(tx, ok, &info.ST2)
			return ok
		}
		// without crash failure the decision should have arrived.
		// wait to accommodate all messages from others.
		c.DecideAsync(tx, false, &info.ST3)

		go func() {
			configs.Warn(false, fmt.Sprintf("The transaction %v is blocked now, and needs to be resolved by termination Protocol", tx.TxnID))
			res = <-resChan
			//if res == nil {
			//	configs.DPrintf("The transaction %v is dead", tx.TxnID)
			//	c.createIfNotExistTxnHandler(tx.TxnID, configs.FCff, len(tx.Participants))
			//	c.DecideAsync(tx, false, &info.ST3)
			//} else {
			//	configs.DPrintf("The transaction %v is resolved now with results %v", tx.TxnID, res.String())
			//	c.createIfNotExistTxnHandler(tx.TxnID, configs.FCff, len(tx.Participants))
			//	c.DecideAsync(tx, res.DecideAllCommit(), &info.ST3)
			//}
		}()
		return false
	}
}

// FCcfSubmit submit the transaction with FC-CF.
func (c *Manager) FCcfSubmit(tx *TX, info *utils.Info) bool {
	defer c.clearTxnHandler(tx.TxnID)
	if info == nil {
		info.Failure = true
		info = utils.NewInfo(len(tx.Participants))
	}
	tx.Protocol = configs.FCcf
	res := c.FCPropose(tx, &info.ST1, info) // Prepare message + write executions.
	if res == nil {
		// this only happens when there is a crash failure.
		c.DecideAsync(tx, false, &info.ST2)
		// non-blocking decide abort for all participants
		return false
	}
	corr := res.Correct(detector.CFNoNF)
	if !corr {
		info.IncorrectAssumption = true
	}
	// undecided could happen when a node crashes before sending vote
	if res.AppendFinished() && res.Decision == detector.UnDecided {
		// all of them voted yes, since no abort decision received
		c.DecideTransitBeforeDecide(tx, true, &info.ST2)
		return true
	} else if !res.AllPersisted() {
		c.DecideAsync(tx, false, &info.ST2)
	}
	return false
}
