package participant

import (
	"FC/configs"
	"FC/network"
	"FC/storage"
	"context"
	"github.com/magiconair/properties/assert"
	"testing"
	"time"
)

func makeLocal() {
	configs.SetLocal()
	configs.ClientRoutineNumber = 1
	configs.SetDown(0)
	configs.SetFailureInjection(-1, -1)
	configs.SetMinLevel(1)
}

func StopServers(stmt []*Context) {
	for _, v := range stmt {
		v.Close()
	}
}

func CheckPreRead(coh *Manager, txn *network.CoordinatorGossip, ans []string, shardID string) {
	return
	//if len(txn.OptList) > 0 {
	//	txn.OptList = txn.OptList[:0]
	//}
	//for i := 0; i < 5; i++ {
	//	txn.AppendRead("MAIN", shardID, uint64(i))
	//}
	//val, ok := coh.PreRead(txn)
	//configs.Assert(ok && len(val) == len(ans), "PreRead failed with error or inconsistent length")
	//for i := 0; i < len(ans); i++ {
	//	configs.Assert(ans[i] == strconv.Itoa(val["MAIN"+shardID+strconv.Itoa(i)].GetAttribute(0).(int)), "PreRead failed with different value")
	//}
}

func TestBuildTestCasesParticipantBranch(t *testing.T) {
	makeLocal()
	ctx := context.WithValue(context.Background(), "store_list",
		[]string{configs.BenchmarkStorage,
			configs.PostgreSQL,
			configs.MongoDB})
	stmts := TestKit(ctx)
	CheckVal(t, stmts[0].Manager, []string{"3", "4", "5", "6", "7"})
	CheckVal(t, stmts[1].Manager, []string{"3", "4", "5", "6", "7"})
	CheckVal(t, stmts[2].Manager, []string{"3", "4", "5", "6", "7"})
	StopServers(stmts)
}

func TestCohortsPreReadParticipantBranch(t *testing.T) {
	makeLocal()
	ctx := context.WithValue(context.Background(), "store_list",
		[]string{configs.MongoDB,
			configs.PostgreSQL,
			configs.BenchmarkStorage})
	stmts := TestKit(ctx)

	txn1 := network.NewTXPack(1, stmts[0].address, configs.NoACP, address)
	txn2 := network.NewTXPack(2, stmts[1].address, configs.NoACP, address)
	c := make(chan bool)
	go func() {
		CheckPreRead(stmts[1].Manager, txn2, []string{"3", "4", "5", "6", "7"}, stmts[1].address)
		c <- true
	}()
	go func() {
		CheckPreRead(stmts[0].Manager, txn1, []string{"3", "4", "5", "6", "7"}, stmts[0].address)
		c <- true
	}()
	<-c
	<-c
	StopServers(stmts)
}

func TestCohortsPreWriteParticipantBranch(t *testing.T) {
	makeLocal()
	ctx := context.WithValue(context.Background(), "store_list",
		[]string{configs.PostgreSQL,
			configs.MongoDB,
			configs.BenchmarkStorage})
	stmts := TestKit(ctx)

	txn1 := network.NewTXPack(1, stmts[0].address, configs.TwoPC, address)
	CheckPreRead(stmts[0].Manager, txn1, []string{"3", "4", "5", "6", "7"}, stmts[0].address)
	txn1.OptList = txn1.OptList[:0]
	for i := 0; i < 5; i++ {
		txn1.AppendUpdate("MAIN", stmts[0].address, uint64(i), storage.WrapTestValue(i+1))
	}
	//configs.JPrint(txn1)
	ok := stmts[0].Manager.PreWrite(txn1)
	assert.Equal(t, true, ok)
	ok = stmts[0].Manager.Commit(txn1)
	assert.Equal(t, true, ok)
	CheckVal(t, stmts[0].Manager, []string{"1", "2", "3", "4", "5"})
	StopServers(stmts)
}

func Test2PCCommitParticipantBranch(t *testing.T) {
	makeLocal()
	ctx := context.WithValue(context.Background(), "store_list",
		[]string{configs.BenchmarkStorage,
			configs.PostgreSQL,
			configs.MongoDB})
	stmts := TestKit(ctx)

	txn1 := network.NewTXPack(1, stmts[0].address, configs.TwoPC, address)
	CheckPreRead(stmts[0].Manager, txn1, []string{"3", "4", "5", "6", "7"}, stmts[0].address)
	txn2 := network.NewTXPack(1, stmts[1].address, configs.TwoPC, address)
	CheckPreRead(stmts[1].Manager, txn2, []string{"3", "4", "5", "6", "7"}, stmts[1].address)
	txn1.OptList = txn1.OptList[:0]
	txn2.OptList = txn2.OptList[:0]
	for i := 0; i < 5; i++ {
		txn1.AppendUpdate("MAIN", stmts[0].address, uint64(i), storage.WrapTestValue(i+1))
		txn2.AppendUpdate("MAIN", stmts[1].address, uint64(i), storage.WrapTestValue(i+2))
	}
	c := make(chan bool)
	go func() {
		ok := stmts[0].Manager.PreWrite(txn1)
		c <- configs.Assert(ok, "PreWrite Made Failed")
	}()
	go func() {
		ok := stmts[1].Manager.PreWrite(txn2)
		c <- configs.Assert(ok, "PreWrite Made Failed")
	}()
	<-c
	<-c
	go func() {
		ok := stmts[0].Manager.Commit(txn1)
		c <- configs.Assert(ok, "Commit Made Failed")
	}()
	go func() {
		ok := stmts[1].Manager.Commit(txn2)
		c <- configs.Assert(ok, "Commit Made Failed")
	}()
	<-c
	<-c
	CheckPreRead(stmts[0].Manager, txn1, []string{"1", "2", "3", "4", "5"}, stmts[0].address)
	CheckVal(t, stmts[0].Manager, []string{"1", "2", "3", "4", "5"})
	CheckVal(t, stmts[1].Manager, []string{"2", "3", "4", "5", "6"})
	StopServers(stmts)
}

func Test2PCAbortParticipantBranch(t *testing.T) {
	makeLocal()
	ctx := context.WithValue(context.Background(), "store_list",
		[]string{configs.BenchmarkStorage,
			configs.PostgreSQL,
			configs.MongoDB})
	stmts := TestKit(ctx)

	txn1 := network.NewTXPack(1, stmts[0].address, configs.TwoPC, address)
	CheckPreRead(stmts[0].Manager, txn1, []string{"3", "4", "5", "6", "7"}, stmts[0].address)
	txn2 := network.NewTXPack(1, stmts[1].address, configs.TwoPC, address)
	CheckPreRead(stmts[1].Manager, txn2, []string{"3", "4", "5", "6", "7"}, stmts[1].address)
	txn1.OptList = txn1.OptList[:0]
	txn2.OptList = txn2.OptList[:0]
	for i := 0; i < 5; i++ {
		txn1.AppendUpdate("MAIN", stmts[0].address, uint64(i), storage.WrapTestValue(i+1))
		txn2.AppendUpdate("MAIN", stmts[1].address, uint64(i), storage.WrapTestValue(i+1))
	}
	stmts[0].Manager.PreWrite(txn1)
	stmts[1].Manager.PreWrite(txn2)
	ok := stmts[0].Manager.Abort(txn1)
	ok = ok && stmts[1].Manager.Abort(txn2) // if the first term is false, the commit will not be executed.
	configs.Assert(ok, "Abort Made Failed")
	CheckVal(t, stmts[0].Manager, []string{"3", "4", "5", "6", "7"})
	CheckPreRead(stmts[1].Manager, txn2, []string{"3", "4", "5", "6", "7"}, stmts[1].address)
	StopServers(stmts)
}

func Test3PCCommitParticipantBranch(t *testing.T) {
	makeLocal()
	ctx := context.WithValue(context.Background(), "store_list",
		[]string{configs.BenchmarkStorage,
			configs.PostgreSQL,
			configs.MongoDB})
	stmts := TestKit(ctx)

	txn1 := network.NewTXPack(1, stmts[0].address, configs.ThreePC, address)
	CheckPreRead(stmts[0].Manager, txn1, []string{"3", "4", "5", "6", "7"}, stmts[0].address)
	txn2 := network.NewTXPack(1, stmts[1].address, configs.ThreePC, address)
	CheckPreRead(stmts[1].Manager, txn2, []string{"3", "4", "5", "6", "7"}, stmts[1].address)
	txn1.OptList = txn1.OptList[:0]
	txn2.OptList = txn2.OptList[:0]
	for i := 0; i < 5; i++ {
		txn1.AppendUpdate("MAIN", stmts[0].address, uint64(i), storage.WrapTestValue(i+1))
		txn2.AppendUpdate("MAIN", stmts[1].address, uint64(i), storage.WrapTestValue(i+2))
	}
	c := make(chan bool)
	go func() {
		ok := stmts[0].Manager.PreWrite(txn1)
		c <- configs.Assert(ok, "PreWrite Made Failed")
	}()
	go func() {
		ok := stmts[1].Manager.PreWrite(txn2)
		c <- configs.Assert(ok, "PreWrite Made Failed")
	}()
	<-c
	<-c
	go func() {
		ok := stmts[0].Manager.PreCommit(txn1)
		c <- configs.Assert(ok, "pre-commit failed")
	}()
	go func() {
		ok := stmts[1].Manager.PreCommit(txn2)
		c <- configs.Assert(ok, "pre-commit failed")
	}()
	<-c
	<-c
	go func() {
		ok := stmts[0].Manager.Commit(txn1)
		c <- configs.Assert(ok, "Commit Made Failed")
	}()
	go func() {
		ok := stmts[1].Manager.Commit(txn2)
		c <- configs.Assert(ok, "Commit Made Failed")
	}()
	<-c
	<-c
	CheckPreRead(stmts[0].Manager, txn1, []string{"1", "2", "3", "4", "5"}, stmts[0].address)
	CheckVal(t, stmts[0].Manager, []string{"1", "2", "3", "4", "5"})
	CheckVal(t, stmts[1].Manager, []string{"2", "3", "4", "5", "6"})
	StopServers(stmts)
}

func Test3PCAbortParticipantBranch(t *testing.T) {
	makeLocal()
	ctx := context.WithValue(context.Background(), "store_list",
		[]string{configs.BenchmarkStorage,
			configs.PostgreSQL,
			configs.MongoDB})
	stmts := TestKit(ctx)

	txn1 := network.NewTXPack(1, stmts[0].address, configs.ThreePC, address)
	CheckPreRead(stmts[0].Manager, txn1, []string{"3", "4", "5", "6", "7"}, stmts[0].address)
	txn2 := network.NewTXPack(1, stmts[1].address, configs.ThreePC, address)
	CheckPreRead(stmts[1].Manager, txn2, []string{"3", "4", "5", "6", "7"}, stmts[1].address)
	txn1.OptList = txn1.OptList[:0]
	txn2.OptList = txn2.OptList[:0]
	for i := 0; i < 5; i++ {
		txn1.AppendUpdate("MAIN", stmts[0].address, uint64(i), storage.WrapTestValue(i+1))
		txn2.AppendUpdate("MAIN", stmts[1].address, uint64(i), storage.WrapTestValue(i+2))
	}
	c := make(chan bool)
	go func() {
		ok := stmts[0].Manager.PreWrite(txn1)
		c <- configs.Assert(ok, "PreWrite Made Failed")
	}()
	go func() {
		ok := stmts[1].Manager.PreWrite(txn2)
		c <- configs.Assert(ok, "PreWrite Made Failed")
	}()
	<-c
	<-c
	go func() {
		ok := stmts[0].Manager.Abort(txn1)
		c <- configs.Assert(ok, "pre-commit failed")
	}()
	go func() {
		ok := stmts[1].Manager.Abort(txn2)
		c <- configs.Assert(ok, "pre-commit failed")
	}()
	<-c
	<-c
	CheckPreRead(stmts[0].Manager, txn1, []string{"3", "4", "5", "6", "7"}, stmts[0].address)
	CheckVal(t, stmts[0].Manager, []string{"3", "4", "5", "6", "7"})
	CheckVal(t, stmts[1].Manager, []string{"3", "4", "5", "6", "7"})
	StopServers(stmts)
}

func TestECAbortParticipantBranch(t *testing.T) {
	makeLocal()
	ctx := context.WithValue(context.Background(), "store_list",
		[]string{configs.BenchmarkStorage,
			configs.PostgreSQL,
			configs.MongoDB})
	stmts := TestKit(ctx)

	txn1 := network.NewTXPack(1, stmts[0].address, configs.EasyCommit, address)
	CheckPreRead(stmts[0].Manager, txn1, []string{"3", "4", "5", "6", "7"}, stmts[0].address)
	txn2 := network.NewTXPack(1, stmts[1].address, configs.EasyCommit, address)
	CheckPreRead(stmts[1].Manager, txn2, []string{"3", "4", "5", "6", "7"}, stmts[1].address)
	txn1.OptList = txn1.OptList[:0]
	txn2.OptList = txn2.OptList[:0]
	for i := 0; i < 5; i++ {
		txn1.AppendUpdate("MAIN", stmts[0].address, uint64(i), storage.WrapTestValue(i+1))
		txn2.AppendUpdate("MAIN", stmts[1].address, uint64(i), storage.WrapTestValue(i+2))
	}
	c := make(chan bool)
	go func() {
		ok := stmts[0].Manager.PreWrite(txn1)
		c <- configs.Assert(ok, "PreWrite Made Failed")
	}()
	go func() {
		ok := stmts[1].Manager.PreWrite(txn2)
		c <- configs.Assert(ok, "PreWrite Made Failed")
	}()
	<-c
	<-c
	go func() {
		ok := stmts[0].Manager.Abort(txn1)
		c <- configs.Assert(ok, "pre-commit failed")
	}()
	go func() {
		ok := stmts[1].Manager.Abort(txn2)
		c <- configs.Assert(ok, "pre-commit failed")
	}()
	<-c
	<-c
	CheckPreRead(stmts[0].Manager, txn1, []string{"3", "4", "5", "6", "7"}, stmts[0].address)
	CheckVal(t, stmts[0].Manager, []string{"3", "4", "5", "6", "7"})
	CheckVal(t, stmts[1].Manager, []string{"3", "4", "5", "6", "7"})
	StopServers(stmts)
}

func TestECCommitParticipantBranch(t *testing.T) {
	makeLocal()
	ctx := context.WithValue(context.Background(), "store_list",
		[]string{configs.BenchmarkStorage,
			configs.PostgreSQL,
			configs.MongoDB})
	stmts := TestKit(ctx)

	txn1 := network.NewTXPack(1, stmts[0].address, configs.EasyCommit, address)
	CheckPreRead(stmts[0].Manager, txn1, []string{"3", "4", "5", "6", "7"}, stmts[0].address)
	txn2 := network.NewTXPack(1, stmts[1].address, configs.EasyCommit, address)
	CheckPreRead(stmts[1].Manager, txn2, []string{"3", "4", "5", "6", "7"}, stmts[1].address)
	txn1.OptList = txn1.OptList[:0]
	txn2.OptList = txn2.OptList[:0]
	for i := 0; i < 5; i++ {
		txn1.AppendUpdate("MAIN", stmts[0].address, uint64(i), storage.WrapTestValue(i+1))
		txn2.AppendUpdate("MAIN", stmts[1].address, uint64(i), storage.WrapTestValue(i+2))
	}
	c := make(chan bool)
	go func() {
		ok := stmts[0].Manager.PreWrite(txn1)
		c <- configs.Assert(ok, "PreWrite Made Failed")
	}()
	go func() {
		ok := stmts[1].Manager.PreWrite(txn2)
		c <- configs.Assert(ok, "PreWrite Made Failed")
	}()
	<-c
	<-c
	go func() {
		ok := stmts[0].Manager.Commit(txn1)
		c <- configs.Assert(ok, "pre-commit failed")
	}()
	go func() {
		ok := stmts[1].Manager.Commit(txn2)
		c <- configs.Assert(ok, "pre-commit failed")
	}()
	<-c
	<-c
	CheckPreRead(stmts[0].Manager, txn1, []string{"1", "2", "3", "4", "5"}, stmts[0].address)
	CheckVal(t, stmts[0].Manager, []string{"1", "2", "3", "4", "5"})
	CheckVal(t, stmts[1].Manager, []string{"2", "3", "4", "5", "6"})
	StopServers(stmts)
}

func TestFCCommitParticipantBranchQuickPath(t *testing.T) {
	makeLocal()
	ctx := context.WithValue(context.Background(), "store_list",
		[]string{configs.BenchmarkStorage,
			configs.PostgreSQL,
			configs.MongoDB})
	stmts := TestKit(ctx)

	txn1 := network.NewTXPack(1, stmts[0].address, configs.FCff, address[:2])
	CheckPreRead(stmts[0].Manager, txn1, []string{"3", "4", "5", "6", "7"}, stmts[0].address)
	txn2 := network.NewTXPack(1, stmts[1].address, configs.FCff, address[:2])
	CheckPreRead(stmts[1].Manager, txn2, []string{"3", "4", "5", "6", "7"}, stmts[1].address)
	txn1.OptList = txn1.OptList[:0]
	txn2.OptList = txn2.OptList[:0]
	for i := 0; i < 5; i++ {
		txn1.AppendUpdate("MAIN", stmts[0].address, uint64(i), storage.WrapTestValue(i+1))
		txn2.AppendUpdate("MAIN", stmts[1].address, uint64(i), storage.WrapTestValue(i+2))
	}
	c := make(chan bool)
	go func() {
		ok := stmts[0].Manager.Propose(txn1, time.Now())
		c <- configs.Assert(ok.Committed() && ok.Persisted, "PreWrite Made Failed")
	}()
	go func() {
		ok := stmts[1].Manager.Propose(txn2, time.Now())
		c <- configs.Assert(ok.Committed() && ok.Persisted, "PreWrite Made Failed")
	}()
	<-c
	<-c
	CheckPreRead(stmts[0].Manager, txn1, []string{"1", "2", "3", "4", "5"}, stmts[0].address)
	CheckVal(t, stmts[0].Manager, []string{"1", "2", "3", "4", "5"})
	CheckVal(t, stmts[1].Manager, []string{"2", "3", "4", "5", "6"})
	StopServers(stmts)
}

func TestFCNodeCrashes(t *testing.T) {
	makeLocal()
	ctx := context.WithValue(context.Background(), "store_list",
		[]string{configs.BenchmarkStorage,
			configs.PostgreSQL,
			configs.MongoDB})
	stmts := TestKit(ctx)

	txn1 := network.NewTXPack(1, stmts[0].address, configs.FCff, address[:2])
	CheckPreRead(stmts[0].Manager, txn1, []string{"3", "4", "5", "6", "7"}, stmts[0].address)
	txn2 := network.NewTXPack(1, stmts[1].address, configs.FCff, address[:2])
	CheckPreRead(stmts[1].Manager, txn2, []string{"3", "4", "5", "6", "7"}, stmts[1].address)
	txn1.OptList = txn1.OptList[:0]
	txn2.OptList = txn2.OptList[:0]
	for i := 0; i < 5; i++ {
		txn1.AppendUpdate("MAIN", stmts[0].address, uint64(i), storage.WrapTestValue(i+1))
		txn2.AppendUpdate("MAIN", stmts[1].address, uint64(i), storage.WrapTestValue(i+2))
	}
	c := make(chan bool)
	go func() {
		ok := stmts[0].Manager.Propose(txn1, time.Now())
		c <- configs.Assert(!ok.Persisted, "propose shall not be persisted without all Yes votes")
	}()
	<-c
	stmts[0].Manager.Abort(txn1)
	CheckPreRead(stmts[0].Manager, txn1, []string{"1", "2", "3", "4", "5"}, stmts[0].address)
	CheckVal(t, stmts[0].Manager, []string{"3", "4", "5", "6", "7"})
	CheckVal(t, stmts[1].Manager, []string{"3", "4", "5", "6", "7"})
	StopServers(stmts)
}
