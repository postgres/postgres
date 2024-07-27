package coordinator

import (
	"FC/configs"
	"FC/network"
	"FC/network/detector"
	"FC/storage"
	"fmt"
	"strconv"
	"sync"
	"time"
)

type txnHandler struct {
	latch    *sync.Mutex
	State    uint8
	TID      uint64
	MsgPool  []interface{}
	MsgCount int
	// if the msg content is not used, msg count is used instead.
	// for replicate protocols MsgCount is increased only when enough votes from replicas collected.
	from        *Manager
	VoterNumber int
	Protocol    string
	// used by replicated ACPs, vote ACK.
	VoteACKs map[string]int
	// used by replicated ACPs, agree ACK.
	AgreeACKs map[string]int
	// finish channel to trigger next phase.
	finish chan struct{}
}

func newTxnHandler(tid uint64, proto string, from *Manager, voteN int) *txnHandler {
	res := &txnHandler{
		latch:       &sync.Mutex{},
		MsgPool:     make([]interface{}, voteN),
		from:        from,
		Protocol:    proto,
		State:       None,
		TID:         tid,
		VoterNumber: voteN,
		MsgCount:    0,
		// asynchronous message handling
		finish: make(chan struct{}, 1),
	}
	if proto == configs.GPAC {
		res.VoteACKs = make(map[string]int)
		res.AgreeACKs = make(map[string]int)
	}
	return res
}

func (c *Manager) createIfNotExistTxnHandler(tid uint64, proto string, voterNumber int) *txnHandler {
	tx, ok := c.TxnPool.Load(tid)
	if !ok {
		configs.TPrintf("TXN" + strconv.FormatUint(tid, 10) + ": " + "transaction branch created on coordinator")
		tx = newTxnHandler(tid, proto, c, voterNumber)
		c.TxnPool.Store(tid, tx)
	}
	return tx.(*txnHandler)
}

func (c *Manager) clearTxnHandler(tid uint64) {
	c.TxnPool.Delete(tid)
}

func (c *Manager) mustExistTxnHandler(tid uint64) *txnHandler {
	tx, ok := c.TxnPool.Load(tid)
	configs.Assert(ok, "the transaction handler must exist")
	return tx.(*txnHandler)
}

func (c *Manager) ignoreIfNotExistTxnHandler(tid uint64) *txnHandler {
	tx, ok := c.TxnPool.Load(tid)
	if !ok {
		return nil
	}
	return tx.(*txnHandler)
}

func (c *txnHandler) transit(begin uint8, end uint8) bool {
	c.latch.Lock()
	defer c.latch.Unlock()
	if c.State == end {
		return true
	}
	if c.State != begin {
		panic(fmt.Sprintf("incorrect State %v for TXN%v", c.State, c.TID))
	}
	c.State = end
	return true
}

func (c *txnHandler) extractResult() map[string]*storage.RowData {
	c.latch.Lock()
	defer c.latch.Unlock()
	res := make(map[string]*storage.RowData)
	for _, msg := range c.MsgPool {
		mp := msg.(map[string]*storage.RowData)
		for k, v := range mp {
			res[k] = v
		}
	}
	return res
}

func (c *txnHandler) extractFCResult() *detector.KvResult {
	c.latch.Lock()
	defer c.latch.Unlock()
	ans := detector.NewKvResult(c.VoterNumber)
	for _, v := range c.MsgPool {
		if v == nil {
			return nil
		}
		tmp := v.(detector.KvRes)
		//configs.JPrint(tmp)
		ok := ans.Append(&tmp)
		if !configs.Assert(ok, "The append is caught with failure") {
			return nil
		}
	}
	return ans
}

func (c *txnHandler) clearMsgPool() {
	c.latch.Lock()
	defer c.latch.Unlock()
	c.MsgPool = c.MsgPool[:0]
	if configs.SelectedACP != configs.GPAC {
		c.VoteACKs = make(map[string]int)
		c.AgreeACKs = make(map[string]int)
	}
	c.MsgCount = 0
}

func (c *txnHandler) canCommitWithAllVotes() bool {
	c.latch.Lock()
	defer c.latch.Unlock()
	return c.MsgCount == c.VoterNumber && (c.State == Committed ||
		(c.State == AgCommitted && c.Protocol == configs.ThreePC) ||
		(c.State == AgCommitted && c.Protocol == configs.PAC) ||
		(c.State == TransitCommit && c.Protocol == configs.EasyCommit) ||
		(c.State == PreWrite && c.Protocol == configs.LearnedC))
}

func (c *txnHandler) quorumACKCollected() bool {
	c.latch.Lock()
	defer c.latch.Unlock()
	return c.MsgCount >= c.VoterNumber/2+1
}

func (c *txnHandler) allACKCollected() bool {
	c.latch.Lock()
	defer c.latch.Unlock()
	return c.MsgCount == c.VoterNumber
}

func (c *txnHandler) ftPreWrite() bool {
	c.latch.Lock()
	defer c.latch.Unlock()
	return c.State == AgCommitted
}

func (c *txnHandler) haveCommitted() bool {
	c.latch.Lock()
	defer c.latch.Unlock()
	return c.State == Committed
}

func (c *txnHandler) handleResponse(msg *network.Response4Coordinator) {
	//// thread-unsafe access, serve as the fast path.
	//if (c.State == Aborted || c.State == Committed) && msg.Mark != configs.Finished {
	//	// reject messages no longer needed.
	//	return
	//}
	//configs.JPrint(msg)
	//configs.JPrint(c.VoteACKs)
	if msg.Mark == configs.PreWriteACK || msg.Mark == configs.FCResults {
		// the timeout in the voting phase.
		duration := time.Since(msg.BeginTime)
		c.from.stmt.UpdateNetworkDelay(msg.From, duration, msg.Mark)
	}
	c.latch.Lock()
	switch msg.Mark {
	case configs.PreWriteACK:
		if c.State == Aborted || c.State == TransitAbort {
			// the transaction has been aborted by a no vote or an asynchronous replica in G-PAC.
			c.latch.Unlock()
			return
		}
		if c.State != PreWrite {
			// have decided
			if c.Protocol != configs.GPAC {
				// when replication enabled, protocols like G-PAC does not require the vote from all participants.
				c.State = Abnormal
				panic(fmt.Sprintf("unexpected State %v for TXN%v", c.State, c.TID))
			}
			c.latch.Unlock()
			return
		}
		increase := false
		hasVoted := false
		if c.Protocol == configs.GPAC {
			c.VoteACKs[msg.ShardID]++
			if len(c.VoteACKs) > configs.ShardsPerTransaction {
				configs.JPrint(c)
				configs.JPrint(msg)
				panic("impossible case: shard number > 2")
			}
			//configs.JPrint(c.VoteACKs)
			// /configs.NumberOfReplicas/2+1
			if c.VoteACKs[msg.ShardID] == configs.NumberOfReplicas/2+1 {
				// the superset of replicas.
				c.MsgCount++
				increase = true
			} else if c.VoteACKs[msg.ShardID] > configs.NumberOfReplicas/2+1 {
				hasVoted = true
			}
		} else {
			c.MsgCount++
			increase = true
		}
		if !msg.ACK && !hasVoted {
			if c.Protocol == configs.EasyCommit {
				c.State = TransitAbort
			} else {
				c.State = Aborted
			}
			c.latch.Unlock()
			c.finish <- struct{}{}
		} else if c.MsgCount == c.VoterNumber && increase {
			if c.Protocol == configs.TwoPC || c.Protocol == configs.NoACP {
				c.State = Committed
			} else if c.Protocol == configs.ThreePC || c.Protocol == configs.PAC || c.Protocol == configs.GPAC {
				c.State = AgCommitted
			} else if c.Protocol == configs.EasyCommit {
				c.State = TransitCommit
			} else if c.Protocol == configs.LearnedC {
				// Do nothing.
			}
			c.latch.Unlock()
			// avoid recall this part in replicated settings.
			c.finish <- struct{}{}
		} else {
			c.latch.Unlock()
		}
	case configs.Finished:
		if c.State != Committed && c.State != Aborted {
			c.State = Abnormal
			if msg.ACK {
				c.State = Committed
			} else {
				c.State = Aborted
			}
			c.latch.Unlock()
			//panic(fmt.Sprintf("unexpected State for TXN%v", c.TID))
			return
		}
		//if c.Protocol == configs.PAC || c.Protocol == configs.GPAC || c.Protocol == configs.ThreePC {
		//	c.latch.Unlock()
		//	return
		//}
		if msg.ACK != (c.State == Committed) && (c.Protocol != configs.GPAC) {
			// G-PAC may return different State due to the asynchronous execution on replications.
			c.State = Abnormal
			c.latch.Unlock()
			panic(fmt.Sprintf("violation of atomic property for TXN%v", msg.TID))
			return
		}
		increase := false
		//configs.JPrint(c.VoteACKs)
		if c.Protocol == configs.GPAC {
			c.VoteACKs[msg.ShardID]++ // Possible ERROR: The shardID here is not shard ID but the sender ID?
			//configs.JPrint(c.VoteACKs)
			// /configs.NumberOfReplicas/2+1
			//if c.VoteACKs[msg.ShardID] == configs.NumberOfReplicas {
			if c.VoteACKs[msg.ShardID] == configs.NumberOfReplicas/2+1 {
				// the superset of replicas.
				c.MsgCount++
				increase = true
			}
		} else {
			c.MsgCount++
			increase = true
		}
		//c.MsgPool = append(c.MsgPool, msg.ACK)
		if c.Protocol == configs.FCff {
			c.latch.Unlock()
		} else if c.Protocol == configs.TwoPC || c.Protocol == configs.LearnedC || c.Protocol == configs.ThreePC || (c.Protocol == configs.GPAC && increase) {
			// all ACKs required.
			if c.MsgCount == c.VoterNumber && increase {
				c.latch.Unlock()
				c.finish <- struct{}{}
			} else {
				c.latch.Unlock()
			}
		} else {
			c.latch.Unlock()
		}
	case configs.PreCommitACK:
		//configs.JPrint(c.VoteACKs)
		//configs.JPrint(c.State)
		if (c.State == Committed || c.State == Aborted) && c.Protocol == configs.GPAC {
			c.latch.Unlock()
			return
		}
		if c.State != AgCommitted {
			c.State = Abnormal
			c.latch.Unlock()
			panic("pre-commit unsuccessful")
			return
		}
		if !msg.ACK {
			// a node cannot enter pre-commit, abort the transaction.
			if c.Protocol == configs.GPAC {
				panic("invalid cache: pre-commit return fail in FT-agree")
			}
			c.State = Aborted
			c.latch.Unlock()
			c.finish <- struct{}{}
		} else {
			// wait for all pre-commit success to continue.
			increase := false
			if c.Protocol == configs.GPAC {
				c.AgreeACKs[msg.ShardID]++
				if c.AgreeACKs[msg.ShardID] == configs.NumberOfReplicas/2+1 {
					// the superset of replicas.
					c.MsgCount++
					increase = true
				}
			} else {
				c.MsgCount++
				increase = true
			}
			// FT-agreement for the PAC/G-PAC and all agreement for the 3PC.
			if (c.MsgCount == c.VoterNumber && increase) || ((c.Protocol == configs.PAC || c.Protocol == configs.GPAC) && c.MsgCount == c.VoterNumber/2+1 && increase) {
				c.State = Committed
				c.latch.Unlock()
				c.finish <- struct{}{}
			} else {
				c.latch.Unlock()
			}
		}
	case configs.ReadUnsuccessful, configs.ReadSuccess:
		if c.State != PreRead {
			// the pre-read has already failed.
			//c.State = Abnormal
			c.latch.Unlock()
			//panic("invalid State")
			return
		}
		ok := msg.Mark == configs.ReadSuccess
		if !ok {
			c.State = Aborted
			c.MsgPool = c.MsgPool[:0]
			c.latch.Unlock()
			c.finish <- struct{}{}
			return
		}
		c.MsgPool = append(c.MsgPool, msg.Read)
		//configs.JPrint(c.MsgPool)
		c.MsgCount++
		if len(c.MsgPool) == c.VoterNumber {
			c.State = PreWrite
			c.latch.Unlock()
			c.finish <- struct{}{}
		} else {
			c.latch.Unlock()
		}
	case configs.FCResults:
		if c.State != Propose && c.State != Aborted {
			c.State = Abnormal
			c.latch.Unlock()
			panic("invalid State")
			return
		}
		if msg.Res.VoteCommit == false {
			c.State = Aborted
		}
		c.MsgPool = append(c.MsgPool, msg.Res)
		if len(c.MsgPool) == c.VoterNumber {
			if c.State == Propose {
				c.State = Committed
			}
			c.latch.Unlock()
			c.finish <- struct{}{}
		} else {
			c.latch.Unlock()
		}
	default:
		panic("invalid mark received on the coordinator")
	}
}
