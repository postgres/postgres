package participant

import (
	"FC/configs"
	"FC/network"
	"FC/network/detector"
	"FC/storage"
	"encoding/json"
	"strconv"
	"sync"
	"time"
)

type handler struct {
	latch       *sync.Mutex
	finish      chan struct{}
	voteCnt     int
	expectedCnt int
	canAbort    bool
}

func (c *Manager) sendKvVote(aim string, vote network.Gossip, sentTimeFromCoordinator time.Time) {
	configs.Warn(!configs.EnableReplication, "the key-value vote is not supported for replicated protocols now")
	msg := network.PaGossip{
		Mark:      configs.FCGossipVotes,
		Vt:        &vote,
		BeginTime: sentTimeFromCoordinator,
	}
	msgBytes, err := json.Marshal(msg)
	configs.CheckError(err)

	if c.stmt.conn != nil {
		c.stmt.conn.sendMsg(aim, msgBytes)
	}
}

func (c *Manager) createIfNotExistMsgPool(shard string, tid uint64, expectedVotes int) *handler {
	msg, ok := c.msgPool[shard].Load(tid)
	if !ok {
		msg = &handler{canAbort: false, voteCnt: 0, finish: make(chan struct{}, 1),
			latch: &sync.Mutex{}, expectedCnt: expectedVotes}
		c.msgPool[shard].Store(tid, msg)
	}
	return msg.(*handler)
}

func (c *Manager) ignoreIfNotExistMsgPool(shard string, tid uint64) *handler {
	msg, ok := c.msgPool[shard].Load(tid)
	if !ok {
		return nil
	}
	return msg.(*handler)
}

// sendBackCA send back the response message to the coordinator.
func (c *Manager) sendBackCA(TID uint64, shardID, mark string, value interface{}, beginT time.Time) {
	configs.DPrintf("TXN" + strconv.FormatUint(TID, 10) + ": " + "send back message from " + c.stmt.address + ":" + shardID + " to " + c.stmt.coordinator + " with Mark " + mark)
	msg := network.Response4Coordinator{Mark: mark, TID: TID, ShardID: shardID, From: c.stmt.address, BeginTime: beginT}
	switch mark {
	case configs.ReadUnsuccessful, configs.ReadSuccess:
		msg.Read = value.(map[string]*storage.RowData)
	case configs.Finished, configs.PreCommitACK, configs.PreWriteACK:
		msg.ACK = value.(bool)
	case configs.FCResults:
		msg.Res = value.(detector.KvRes)
	}
	msgBytes, err := json.Marshal(msg)
	configs.CheckError(err)
	c.stmt.conn.sendMsg(c.stmt.coordinator, msgBytes)
}

// broadCastVote broadcast votes to other Manager managers.
func (c *Manager) broadCastVote(msg *network.CoordinatorGossip, vote int, CID string, parts []string, sentTime time.Time) {
	vt := network.Gossip{
		TID:         msg.TxnID,
		Protocol:    msg.Protocol,
		VoteCommit:  vote == 1,
		From:        CID,
		ShardNumber: len(parts),
	}
	if msg.Protocol == configs.EasyCommit {
		// if the level = EasyCommit, then it serves as the transmission-and-decide technique.
		for _, tp := range parts {
			if tp != CID { // send and then decide.
				c.sendKvVote(tp, vt, sentTime)
			}
		}
	} else {
		configs.Assert(msg.Protocol == configs.FCff || msg.Protocol == configs.FCcf, "only fc protocol requires broadcast")
		for _, tp := range parts {
			if tp == CID {
				c.handleVote(&vt, 0)
			} else {
				go c.sendKvVote(tp, vt, sentTime)
			}
		}
	}
}

// handleVote handles the vote with message buffer for FCff or handles the decision from other participants.
func (c *Manager) handleVote(vt *network.Gossip, duration time.Duration) {
	buf := c.createIfNotExistMsgPool(c.stmt.address, vt.TID, vt.ShardNumber)
	tx := c.tryLoadTxnBranch(c.stmt.address, vt.TID)
	configs.Assert(vt.ShardNumber == buf.expectedCnt, "incorrect expected count")
	configs.TxnPrint(vt.TID, vt.Protocol+" Vote get From "+vt.From+" to "+c.stmt.address)
	c.stmt.UpdateNetworkDelay(vt.From, duration)
	if vt.Protocol == configs.EasyCommit {
		// only transit-then-decide messages are marked with EC, make the same decision with others.
		if buf == nil {
			return
		} // the buffer get concurrently cleared by the finish of local transaction branch.
		if tx != nil {
			if vt.VoteCommit {
				tx.Commit()
			} else {
				tx.Abort()
			}
		} else {
			c.msgPool[c.stmt.address].Delete(vt.TID)
		}
	} else if vt.Protocol == configs.FCff || vt.Protocol == configs.FCcf {
		// the broadcast vote from others, only happen when using FCff*
		buf.latch.Lock()
		if buf.canAbort {
			// the transaction has been aborted
			buf.latch.Unlock()
		} else {
			buf.voteCnt++
			if !vt.VoteCommit {
				buf.canAbort = true
				buf.latch.Unlock()
				buf.finish <- struct{}{}
			} else if buf.voteCnt == buf.expectedCnt {
				// committed with all votes
				buf.latch.Unlock()
				buf.finish <- struct{}{}
			} else {
				buf.latch.Unlock()
			}
		}
	}
}
