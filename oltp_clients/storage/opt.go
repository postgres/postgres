package storage

import (
	"strconv"
)

const (
	ReadOpt   uint = 1
	UpdateOpt uint = 2
)

// TXOpt transaction operations.
type TXOpt struct {
	Type  uint
	Key   uint64
	Shard string
	Table string
	Value *RowData
}

func (r *TXOpt) GetKey() string {
	return r.Table + "$" + r.Shard + "$" + strconv.FormatUint(r.Key, 10)
}
