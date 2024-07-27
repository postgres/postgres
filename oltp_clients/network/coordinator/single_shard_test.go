package coordinator

import (
	"FC/configs"
	"FC/network/participant"
	"FC/storage"
	"fmt"
	"strconv"
	"testing"
)

var address = []string{"127.0.0.1:6001", "127.0.0.1:6002", "127.0.0.1:6003"}

var buf = true

func makeLocal() {
	configs.SetLocal()
	configs.ClientRoutineNumber = 10
	configs.SetDown(0)
	configs.SetFailureInjection(-1, -1)
	configs.SetMinLevel(1)
	configs.StoredProcedure = true
	configs.DetectorInitWaitCnt = 1
	buf = configs.LocalTest
	address = make([]string, 0)
	for i := 0; i < configs.NumberOfShards; i++ {
		address = append(address, fmt.Sprintf("127.0.0.1:60%02d", i+1))
	}
}

func recLocal() {
	configs.LocalTest = buf
	configs.EnableReplication = false
}

func TestSingle(t *testing.T) {
	makeLocal()
	defer recLocal()
	ca, cohorts := TestKit()
	tx := NewTX(1, address[:1], ca.Manager)
	for i := 0; i < 5; i++ {
		tx.AddUpdate("MAIN", address[0], uint64(i), storage.WrapTestValue(i+2))
	}
	res := ca.Manager.SingleSubmit(tx, nil)
	configs.JPrint(res)
	configs.Assert(res, "The one-shoot Failed")
	participant.CheckVal(t, cohorts[0].Manager, []string{"2", "3", "4", "5", "6"})
	participant.CheckVal(t, cohorts[1].Manager, []string{"3", "4", "5", "6", "7"})
	ca.Close()
	for i := 0; i < configs.NumberOfShards; i++ {
		cohorts[i].Close()
	}
}

func TestPreRead(t *testing.T) {
	makeLocal()
	defer recLocal()
	ca, cohorts := TestKit()
	configs.StoredProcedure = false
	txn := NewTX(1, address, ca.Manager)
	participant.CheckVal(t, cohorts[0].Manager, []string{"3", "4", "5", "6", "7"})
	participant.CheckVal(t, cohorts[1].Manager, []string{"3", "4", "5", "6", "7"})
	participant.CheckVal(t, cohorts[2].Manager, []string{"3", "4", "5", "6", "7"})

	for i := uint64(0); i < 5; i++ {
		txn.AddRead("MAIN", address[0], i)
		txn.AddRead("MAIN", address[1], i)
		txn.AddRead("MAIN", address[2], i)
	}
	res, ok := ca.Manager.PreRead(txn)
	configs.Assert(ok, "The PreRead Failed")
	for i := 0; i < 5; i++ {
		for j := 0; j < 3; j++ {
			key := txn.OptList[3*i+j].GetKey()
			configs.Assert(participant.LoadStringValue(res[key].GetAttribute(0)) ==
				strconv.Itoa(i+3), "the value is inconsistent")
		}
	}
	ca.Close()
	for i := 0; i < configs.NumberOfShards; i++ {
		cohorts[i].Close()
	}
}
