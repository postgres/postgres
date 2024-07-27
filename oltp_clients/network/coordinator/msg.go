package coordinator

import (
	"FC/configs"
	"FC/network"
	"encoding/json"
	"strconv"
	"time"
)

func (c *Manager) sendMsg(server string, mark string, txn *network.CoordinatorGossip) {
	configs.DPrintf("TXN" + strconv.FormatUint(txn.TxnID, 10) + ": " + "CA send message for " + server + " with Mark " + mark)
	msg := network.PaGossip{Mark: mark, Txn: txn, BeginTime: time.Now()}
	msgBytes, err := json.Marshal(msg)
	configs.CheckError(err)
	c.stmt.conn.sendMsg(server, msgBytes)
}

func (c *Manager) sendDecide(server string, pack *network.CoordinatorGossip, isCommit bool) {
	if isCommit {
		c.sendMsg(server, configs.Commit, pack)
	} else {
		c.sendMsg(server, configs.Abort, pack)
	}
}

func (c *Manager) sendPreWrite(server string, pack *network.CoordinatorGossip) {
	c.sendMsg(server, configs.PreWrite, pack)
}

func (c *Manager) sendPreCommit(server string, pack *network.CoordinatorGossip) {
	c.sendMsg(server, configs.PreCommit, pack)
}

func (txn *TX) sendPreRead(server string, pack *network.CoordinatorGossip) {
	txn.from.sendMsg(server, configs.PreRead, pack)
}

func (c *Manager) sendPropose(server string, pack *network.CoordinatorGossip) {
	c.sendMsg(server, configs.FCProposed, pack)
}
