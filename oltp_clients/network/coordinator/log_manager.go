package coordinator

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

type TxnLogEntry struct {
	TID   uint64 `json:"TID"`
	State uint8  `json:"State"`
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

func (c *LogManager) writeTxnState(tx *TX, state uint8) {
	if !configs.UseWAL {
		return
	}
	c.latch.Lock()
	defer c.latch.Unlock()
	e := fmt.Sprintf("(t,%v,%v)", tx.TxnID, state)
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
