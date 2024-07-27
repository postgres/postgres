package network

import (
	"FC/network/detector"
	"FC/storage"
	"time"
)

// The first bytes for message type.

// CoordinatorGossip pack transaction information for transportation in ACP.
type CoordinatorGossip struct {
	TxnID    uint64
	ShardID  string
	To       string
	Protocol string
	//NetworkTimeOutWindow int64 // nw[aim] = max{ dis(C,j) + dis(j, aim) | j in ParticipantAddresses}
	EpochNum             uint64
	OptList              []storage.TXOpt
	ParticipantAddresses []string
}

type Gossip struct {
	TID         uint64
	From        string
	VoteCommit  bool
	Protocol    string
	ShardNumber int
}

func (c *PaGossip) String() string {
	return c.Mark
}

type PaGossip struct {
	Mark      string
	Txn       *CoordinatorGossip
	Vt        *Gossip
	BeginTime time.Time
}

type Response4Coordinator struct {
	TID       uint64
	Mark      string
	ShardID   string
	From      string
	Res       detector.KvRes
	Read      map[string]*storage.RowData
	ACK       bool
	BeginTime time.Time
}

// NewReplicatedTXPack create transaction branch for a replica to execute.
func NewReplicatedTXPack(TID uint64, shardID string, replicaID string, proto string, parts []string) *CoordinatorGossip {
	res := &CoordinatorGossip{
		TxnID:                TID,
		ShardID:              shardID,
		To:                   replicaID,
		Protocol:             proto,
		OptList:              make([]storage.TXOpt, 0),
		ParticipantAddresses: parts,
	}
	return res
}

func NewTXPack(TID uint64, shardID string, protocol string, parts []string) *CoordinatorGossip {
	res := &CoordinatorGossip{
		TxnID:                TID,
		ShardID:              shardID,
		To:                   shardID,
		Protocol:             protocol,
		OptList:              make([]storage.TXOpt, 0),
		ParticipantAddresses: parts,
	}
	return res
}

func (c *CoordinatorGossip) AppendRead(table string, shard string, key uint64) {
	c.OptList = append(c.OptList, storage.TXOpt{
		Table: table,
		Shard: shard,
		Key:   key,
		Value: nil,
		Type:  storage.ReadOpt,
	})
}

func (c *CoordinatorGossip) AppendUpdate(table string, shard string, key uint64, value *storage.RowData) {
	c.OptList = append(c.OptList, storage.TXOpt{
		Table: table,
		Shard: shard,
		Key:   key,
		Value: value,
		Type:  storage.UpdateOpt,
	})
}
