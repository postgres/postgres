package utils

import (
	"FC/configs"
	"sync/atomic"
)

var txnID = uint64(0)

func GetTxnID() uint64 {
	return atomic.AddUint64(&txnID, 1) % configs.MaxTID
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
