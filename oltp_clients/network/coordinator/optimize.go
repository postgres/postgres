package coordinator

import (
	"FC/storage"
)

type TX struct {
	TxnID        uint64
	OptList      []storage.TXOpt
	Participants []string
	Protocol     string
	from         *Manager
}

func NewTX(TID uint64, parts []string, from *Manager) *TX {
	return &TX{
		TxnID:        TID,
		OptList:      make([]storage.TXOpt, 0),
		Participants: parts,
		from:         from,
	}
}

func (txn *TX) AddRead(table string, shard string, key uint64) {
	txn.OptList = append(txn.OptList, storage.TXOpt{
		Table: table,
		Shard: shard,
		Key:   key,
		Value: nil,
		Type:  storage.ReadOpt,
	})
}

func (txn *TX) AddUpdate(table string, shard string, key uint64, value *storage.RowData) {
	txn.OptList = append(txn.OptList, storage.TXOpt{
		Table: table,
		Shard: shard,
		Key:   key,
		Value: value,
		Type:  storage.UpdateOpt,
	})
}

//func hashOpt(sd string, key uint64, val int, cmd uint) string {
//	// read write should be supported
//	return sd + ";" + strconv.FormatUint(key, 10) + ";" + strconv.Itoa(val) + ";" + strconv.FormatUint(key, 10)
//}

// Optimize the transaction to reduce useless operations.
// in order to avoid deadlock, we sort operations by the primary key.
func (txn *TX) Optimize() {
	N := len(txn.OptList)
	for i := 0; i < N; i++ {
		for j := i + 1; j < N; j++ {
			if txn.OptList[i].Key > txn.OptList[j].Key {
				txn.OptList[i], txn.OptList[j] = txn.OptList[j], txn.OptList[i]
			}
		}
	}
	//exist := make(map[string]bool)
	//for i := len(txn.OptList) - 1; i >= 0; i-- {
	//	// TODO: currently use int as value, will change to interface.
	//	val := hashOpt(txn.OptList[i].Shard, txn.OptList[i].Key, txn.OptList[i].Value.(int), txn.OptList[i].Type)
	//	if exist[val] {
	//		txn.OptList = append(txn.OptList[:i], txn.OptList[i+1:]...)
	//	} else {
	//		exist[val] = true
	//	}
	//}
}
