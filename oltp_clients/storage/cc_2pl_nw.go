package storage

import (
	"FC/configs"
	"fmt"
	"github.com/viney-shih/go-lock"
)

type LockEntry struct {
	lockType uint8
	txn      *DBTxn
	next     *LockEntry
	prev     *LockEntry
}

type TwoPhaseLockNoWaitManager struct {
	Latch     lock.Mutex
	LockType  uint8
	LockOwner uint32
	Owners    *LockEntry
	OwnerCnt  uint32
	from      *RowRecord
}

func lockCompatible(a, b uint8) bool {
	if a == configs.LockNone || b == configs.LockNone {
		return true
	}
	if a == configs.LockShared && b == configs.LockShared {
		return true
	}
	return false
}

func (c *TwoPhaseLockNoWaitManager) ToString() string {
	c.Latch.Lock()
	defer c.Latch.Unlock()
	if c.Owners == nil {
		return fmt.Sprintf("LatchType:%v; Owner:%v; OwnerCnt:%v\n", c.LockType, "no", c.OwnerCnt)
	}
	return fmt.Sprintf("LatchType:%v; Owner:%v; OwnerCnt:%v\n", c.LockType, c.Owners.txn.txnID, c.OwnerCnt)
}

func (c *TwoPhaseLockNoWaitManager) AccessRow(lockType uint8, txn *DBTxn) uint8 {
	c.Latch.Lock()
	defer c.Latch.Unlock()
	// the transaction try to upgrade/repeat exclusive lock when it has obtained R/W lock.
	if lockType == configs.LockExclusive && c.Owners != nil && c.Owners.txn.txnID == txn.txnID {
		if c.LockType == configs.LockExclusive {
			return configs.LockSucceed
		} else if c.LockType == configs.LockShared && c.OwnerCnt == 1 {
			c.LockType = configs.LockExclusive
			c.Owners.lockType = configs.LockExclusive
			return configs.LockSucceed
		}
	}
	// repeat read shall be cut on the access level.
	ok := lockCompatible(lockType, c.LockType)
	if !ok {
		return configs.LockAbort
	} else {
		entry := &LockEntry{
			lockType: lockType,
			txn:      txn,
			next:     c.Owners,
			prev:     nil,
		}
		if c.Owners != nil {
			c.Owners.prev = entry
		}
		c.Owners = entry
		c.OwnerCnt++
		c.LockType = lockType
		return configs.LockSucceed
	}
}

func (c *TwoPhaseLockNoWaitManager) ReleaseRowLock(lockType uint8, txn *DBTxn) {
	c.Latch.Lock()
	defer c.Latch.Unlock()
	var prev, cur *LockEntry = nil, nil
	for cur = c.Owners; cur != nil && cur.txn.txnID != txn.txnID; cur = cur.next {
		prev = cur
	}
	if cur != nil {
		if prev != nil {
			prev.next = cur.next
		} else {
			c.Owners = cur.next
		}
		if cur.next != nil {
			cur.next.prev = prev
		}
		c.OwnerCnt--
		if c.OwnerCnt == 0 {
			c.LockType = configs.LockNone
		}
	} else {
		//panic("impossible for 2PL no wait")
	}
}

func NewTwoPLNWManager(row *RowRecord) *TwoPhaseLockNoWaitManager {
	return &TwoPhaseLockNoWaitManager{
		from:     row,
		Owners:   nil,
		OwnerCnt: 0,
		LockType: configs.LockNone,
		Latch:    lock.NewCASMutex(),
	}
}
