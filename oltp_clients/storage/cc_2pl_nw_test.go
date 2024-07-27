package storage

import (
	"FC/configs"
	"context"
	"testing"
	"time"
)

const TxnPerThread = 1000
const ThreadNumber = 16

func Test2PLNoWait(t *testing.T) {
	n := ThreadNumber * TxnPerThread
	row := NewRowRecord(nil, 1, 1)
	//fmt.Printf(row.Manager.ToString())
	ch := make(chan bool)
	ctx := context.WithValue(context.Background(), "store", configs.BenchmarkStorage)
	for i := 0; i < ThreadNumber; i++ {
		go func(i int, ch chan bool) {
			txn := NewTxn(ctx)
			for j := i; j < n; j += ThreadNumber {
				txn.txnID = uint32(j)
				rc := row.GetRecord(TxnRead, txn)
				//JPrint(rc.Manager.ToString())
				if rc != nil {
					rc = row.GetRecord(TxnWrite, txn)
					//JPrint(rc.Manager.ToString())
					if rc != nil {
						newRow := &RowRecord{}
						newRow.Copy(row)
						newRow.SetValue(configs.F0, time.Now().String())
						row.ReturnRow(TxnWrite, txn, newRow)
						row.ReturnRow(TxnRead, txn, row)
					} else {
						row.ReturnRow(TxnRead, txn, row)
					}
				} else {
					row.ReturnRow(TxnRead, txn, row)
				}
				//newRow := &RowRecord{}
				//newRow.Copy(row)
				//newRow.SetValue(configs.F0, time.Now().String())
				//row.ReturnRow(TxnWrite, txn, newRow)
				//row.ReturnRow(TxnRead, txn, row)
			}

			ch <- true
		}(i, ch)
	}
	for i := 0; i < ThreadNumber; i++ {
		<-ch
	}
}
