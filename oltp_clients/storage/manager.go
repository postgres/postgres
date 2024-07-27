package storage

import (
	"FC/configs"
	"fmt"
)

type LockManager interface {
	ReleaseRowLock(lockType uint8, txn *DBTxn)
	AccessRow(lockType uint8, txn *DBTxn) uint8
	ToString() string
}

func NewLockManager(row *RowRecord) LockManager {
	switch configs.SelectedCC {
	case configs.TwoPhaseLockNoWait:
		return NewTwoPLNWManager(row)
	case configs.VeryLightLock:
		return NewVLLManager(row)
	default:
		panic(fmt.Sprintf("invalid concurrency control algorithm %v", configs.SelectedCC))
	}
}
