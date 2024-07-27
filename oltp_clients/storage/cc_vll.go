package storage

import (
	"FC/configs"
	"context"
	"github.com/viney-shih/go-lock"
)

type TxnQueueEntry struct {
	prev *TxnQueueEntry
	next *TxnQueueEntry
	txn  *DBTxn
}

type VLLLoop struct {
	latch    lock.Mutex
	ctx      context.Context
	txnQ     *TxnQueueEntry
	txnQTail *TxnQueueEntry
	qSize    uint
	from     *RowRecord
}

func (c *VLLLoop) main() {
}

type VLLManager struct {
	shareCount   uint
	exclusiveCnt uint
	from         *RowRecord
}

func (c *VLLManager) ToString() string {
	return ""
}

func (c *VLLManager) AccessRow(lockType uint8, txn *DBTxn) uint8 {
	if lockType == configs.LockShared {
		c.shareCount++
		if c.exclusiveCnt == 0 {
			return configs.LockSucceed
		} else {
			return configs.LockAbort
		}
	} else {
		c.exclusiveCnt++
		if c.exclusiveCnt == 1 && c.shareCount == 0 {
			return configs.LockSucceed
		} else {
			return configs.LockAbort
		}
	}
}

func (c *VLLManager) ReleaseRowLock(lockType uint8, txn *DBTxn) {
	if lockType == configs.LockShared {
		configs.Assert(c.shareCount > 0, "lock error")
		c.shareCount--
	} else {
		configs.Assert(c.exclusiveCnt > 0, "lock error")
		c.exclusiveCnt--
	}
}

func NewVLLManager(row *RowRecord) *VLLManager {
	return &VLLManager{
		from:         row,
		shareCount:   0,
		exclusiveCnt: 0,
	}
}
