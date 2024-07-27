package coordinator

import (
	"FC/configs"
	"FC/network"
	"FC/storage"
	"FC/utils"
	"time"
)

// OneShoot commit/abort the transaction located in a single shard with 1RRT.
func (c *Manager) OneShoot(txn *TX, info *utils.Info) bool {
	defer configs.TimeLoad(time.Now(), "One shoot", txn.TxnID, &info.Latency)
	handler := c.createIfNotExistTxnHandler(txn.TxnID, configs.NoACP, len(txn.Participants))
	handler.transit(None, PreWrite)
	branches := make(map[string]*network.CoordinatorGossip)
	configs.Assert(len(txn.Participants) == 1, "A cross shard transaction misses atomic commit Protocol")
	for _, v := range txn.Participants {
		branches[v] = network.NewTXPack(txn.TxnID, v, configs.NoACP, txn.Participants)
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
	for i, op := range branches {
		txn.from.sendPreWrite(i, op)
	}
	select {
	case <-time.After(configs.CrashFailureTimeout):
		configs.TPrintf("acp finish after crash failure")
		return false
	case <-c.stmt.ctx.Done():
		configs.TPrintf("acp finish after ctx break")
		return false
	case <-handler.finish:
		return handler.haveCommitted()
	}
}

// SingleSubmit submit transaction to a single KV with one RRT.
func (c *Manager) SingleSubmit(tx *TX, info *utils.Info) bool {
	defer c.clearTxnHandler(tx.TxnID)
	if info == nil {
		info = utils.NewInfo(len(tx.Participants))
	}
	ok := c.OneShoot(tx, info)
	c.TxnPool.Delete(tx.TxnID)
	return ok
}
