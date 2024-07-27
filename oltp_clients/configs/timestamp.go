package configs

import "sync/atomic"

const (
	ZeroValue string = "NULL"
	MaxTID    uint64 = 2000000
)

var txnID = uint64(0)

func GetTxnID() uint64 {
	return atomic.AddUint64(&txnID, 1) % MaxTID
}

func Max(x int, y int) int {
	if x > y {
		return x
	}
	return y
}

func Min(x int, y int) int {
	if x < y {
		return x
	}
	return y
}
