package storage

import (
	"FC/configs"
	"context"
	"fmt"
	"github.com/tidwall/wal"
	"strconv"
	"sync"
	"time"
)

type LogManager struct {
	latch  sync.Mutex
	lsn    uint64
	logs   *wal.Log
	buffer *wal.Batch
	ctx    context.Context
}

type RedoLogEntry struct {
	TID   uint32 `json:"tid"`
	Key   Key    `json:"key"`
	Table string `json:"table"`
	Value string `json:"Value"`
}

type TxnLogEntry struct {
	TID   uint32 `json:"tid"`
	State uint8  `json:"state"`
}

func NewLogManager(shardID string) *LogManager {
	res := &LogManager{}
	if !configs.UseWAL {
		return res
	}
	log, err := wal.Open(fmt.Sprintf("./logs/%s", shardID), nil)
	if err != nil {
		panic(err)
	}
	res.logs = log
	res.lsn, err = log.LastIndex()
	res.buffer = &wal.Batch{}
	if err != nil {
		panic(err)
	}
	if !configs.EnableReplication {
		go res.localBatchSyncLogger(res.ctx, res.lsn)
	} else {
		// TODO: link to replicas.
	}
	return res
}

func (c *LogManager) writeRedoLog4Txn(tx *DBTxn) {
	if !configs.UseWAL {
		return
	}
	c.latch.Lock()
	defer c.latch.Unlock()
	eList := make([]string, 0)
	for i := 0; i < tx.RowCnt; i++ {
		ac := tx.Accesses[i]
		if ac.AccessType == TxnWrite {
			e := fmt.Sprintf("(u,%v,%v,%v,%v)", tx.txnID, ac.Local.FromTable.tableName, ac.Local.PrimaryKey, ac.Local.Data)
			eList = append(eList, e)
		}
	}
	if !configs.EnableReplication {
		for i, v := range eList {
			idx := c.lsn + uint64(i) + 1
			c.buffer.Write(idx, []byte(v))
			configs.JPrint(strconv.FormatUint(idx, 10) + "-" + v)
		}
		c.lsn += uint64(tx.WriteCnt)
	} else {
		// TODO: write logs to replicas.
	}
}

func (c *LogManager) writeTxnState(tx *DBTxn) {
	if !configs.UseWAL {
		return
	}
	c.latch.Lock()
	defer c.latch.Unlock()
	e := fmt.Sprintf("(t,%v,%v)", tx.txnID, tx.TxnState)
	c.lsn++
	if !configs.EnableReplication {
		c.buffer.Write(c.lsn, []byte(e))
		configs.JPrint(strconv.FormatUint(c.lsn, 10) + "-" + e)
	} else {
		// TODO: write logs to replicas.
	}
}

func (c *LogManager) localBatchSyncLogger(ctx context.Context, initLSN uint64) {
	lastLSN := initLSN
	for {
		select {
		case <-time.After(configs.LogBatchInterval):
			c.latch.Lock()
			if c.lsn == lastLSN || c.buffer == nil {
				c.latch.Unlock()
			} else {
				configs.JPrint(c.buffer)
				configs.JPrint(c.lsn)
				configs.JPrint(lastLSN)
				err := c.logs.WriteBatch(c.buffer)
				if err != nil {
					panic(err)
				}
				c.buffer.Clear()
				lastLSN = c.lsn
				c.latch.Unlock()
			}
		case <-ctx.Done():
			break
		}
	}
}
