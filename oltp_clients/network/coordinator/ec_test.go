package coordinator

import (
	"FC/configs"
	"FC/network/participant"
	"FC/storage"
	"testing"
)

func TestECPreWrite(t *testing.T) {
	makeLocal()
	defer recLocal()
	ca, co := TestKit()
	txn := NewTX(1, address, ca.Manager)
	for i := 0; i < 5; i++ {
		txn.AddUpdate("MAIN", address[0], uint64(i), storage.WrapTestValue(i+1))
		txn.AddUpdate("MAIN", address[1], uint64(i), storage.WrapTestValue(i+2))
		txn.AddUpdate("MAIN", address[2], uint64(i), storage.WrapTestValue(i+3))
	}
	res := ca.Manager.EasySubmit(txn, nil)
	configs.Assert(res, "The EC Failed")
	configs.JPrint("EC commit succeed")
	participant.CheckVal(t, co[0].Manager, []string{"1", "2", "3", "4", "5"})
	participant.CheckVal(t, co[1].Manager, []string{"2", "3", "4", "5", "6"})
	participant.CheckVal(t, co[2].Manager, []string{"3", "4", "5", "6", "7"})
	ca.Close()
	for i := 0; i < configs.NumberOfShards; i++ {
		co[i].Close()
	}
}

func TestECSerialOrder(t *testing.T) {
	makeLocal()
	defer recLocal()
	ca, co := TestKit()
	w1 := NewTX(1, address[:2], ca.Manager)
	w2 := NewTX(2, address[1:], ca.Manager)
	configs.JPrint(w1)
	for i := 0; i < 5; i++ {
		w1.AddUpdate("MAIN", address[0], uint64(i), storage.WrapTestValue(i+1))
		w1.AddUpdate("MAIN", address[1], uint64(i), storage.WrapTestValue(i+2))
		w2.AddUpdate("MAIN", address[2], uint64(i), storage.WrapTestValue(i))
		w2.AddUpdate("MAIN", address[1], uint64(i), storage.WrapTestValue(i+2))
	}
	res := ca.Manager.EasySubmit(w1, nil)
	participant.CheckVal(t, co[0].Manager, []string{"1", "2", "3", "4", "5"})
	participant.CheckVal(t, co[1].Manager, []string{"2", "3", "4", "5", "6"})
	participant.CheckVal(t, co[2].Manager, []string{"3", "4", "5", "6", "7"})
	res = res && ca.Manager.EasySubmit(w2, nil)
	configs.Assert(res, "The EC Failed")
	participant.CheckVal(t, co[0].Manager, []string{"1", "2", "3", "4", "5"})
	participant.CheckVal(t, co[1].Manager, []string{"2", "3", "4", "5", "6"})
	participant.CheckVal(t, co[2].Manager, []string{"0", "1", "2", "3", "4"})
	ca.Close()
	for i := 0; i < configs.NumberOfShards; i++ {
		co[i].Close()
	}
}

func TestECConcurrentExecution(t *testing.T) {
	makeLocal()
	defer recLocal()
	ca, co := TestKit()
	w1 := NewTX(1, address[:2], ca.Manager)
	w2 := NewTX(2, address[1:], ca.Manager)
	configs.JPrint(w1)
	for i := 0; i < 5; i++ {
		w1.AddUpdate("MAIN", address[0], uint64(i), storage.WrapTestValue(i+1))
		w1.AddUpdate("MAIN", address[1], uint64(i), storage.WrapTestValue(i+2))
		w2.AddUpdate("MAIN", address[2], uint64(i), storage.WrapTestValue(i))
		w2.AddUpdate("MAIN", address[1], uint64(i), storage.WrapTestValue(i+2))
	}
	ch := make(chan bool)
	go func() {
		res := false
		for !res {
			res = ca.Manager.EasySubmit(w1, nil)
			w1.TxnID += 2
		}
		ch <- res
	}()
	go func() {
		res := false
		for !res {
			res = ca.Manager.EasySubmit(w2, nil)
			w2.TxnID += 2
		}
		ch <- res
	}()
	<-ch
	<-ch
	participant.CheckVal(t, co[0].Manager, []string{"1", "2", "3", "4", "5"})
	participant.CheckVal(t, co[1].Manager, []string{"2", "3", "4", "5", "6"})
	participant.CheckVal(t, co[2].Manager, []string{"0", "1", "2", "3", "4"})
	ca.Close()
	for i := 0; i < configs.NumberOfShards; i++ {
		co[i].Close()
	}
}
