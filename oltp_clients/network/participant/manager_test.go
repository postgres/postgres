package participant

import (
	"FC/configs"
	"FC/network"
	"FC/storage"
	"context"
	"github.com/stretchr/testify/assert"
	"testing"
)

func TestLocalParticipantBranchTxn(t *testing.T) {
	makeLocal()
	ctx := context.WithValue(context.Background(), "store_list",
		[]string{configs.BenchmarkStorage,
			configs.PostgreSQL,
			configs.MongoDB})
	stmts := TestKit(ctx)

	txn1 := network.NewTXPack(1, stmts[0].address, configs.NoACP, configs.OuAddress)
	for i := 0; i < 5; i++ {
		txn1.AppendRead("MAIN", stmts[0].address, uint64(i))
	}
	_, ok := stmts[0].Manager.PreRead(txn1)
	assert.Equal(t, true, ok)
	CheckVal(t, stmts[0].Manager, []string{"3", "4", "5", "6", "7"})

	txn1.OptList = txn1.OptList[:0]
	for i := 0; i < 5; i++ {
		txn1.AppendUpdate("MAIN", stmts[0].address, uint64(i), storage.WrapTestValue(i+1))
	}
	ok = stmts[0].Manager.PreWrite(txn1)
	assert.Equal(t, true, ok)
	CheckVal(t, stmts[0].Manager, []string{"1", "2", "3", "4", "5"})
	StopServers(stmts)
}

func Test2PCParticipantBranchTxn(t *testing.T) {
	makeLocal()
	ctx := context.WithValue(context.Background(), "store_list",
		[]string{configs.BenchmarkStorage,
			configs.PostgreSQL,
			configs.MongoDB})
	stmts := TestKit(ctx)

	txn1 := network.NewTXPack(1, stmts[0].address, configs.TwoPC, configs.OuAddress)
	for i := 0; i < 5; i++ {
		txn1.AppendRead("MAIN", stmts[0].address, uint64(i))
	}
	_, ok := stmts[0].Manager.PreRead(txn1)
	assert.Equal(t, true, ok)
	CheckVal(t, stmts[0].Manager, []string{"3", "4", "5", "6", "7"})

	txn1.OptList = txn1.OptList[:0]
	for i := 0; i < 5; i++ {
		txn1.AppendUpdate("MAIN", stmts[0].address, uint64(i), storage.WrapTestValue(i+1))
	}
	ok = stmts[0].Manager.PreWrite(txn1)
	assert.Equal(t, true, ok)
	ok = stmts[0].Manager.Commit(txn1)
	assert.Equal(t, true, ok)
	CheckVal(t, stmts[0].Manager, []string{"1", "2", "3", "4", "5"})
	StopServers(stmts)
}

func Test2PCParticipantBranchTxnAbort(t *testing.T) {
	makeLocal()
	ctx := context.WithValue(context.Background(), "store_list",
		[]string{configs.BenchmarkStorage,
			configs.PostgreSQL,
			configs.MongoDB})
	stmts := TestKit(ctx)

	txn1 := network.NewTXPack(1, stmts[0].address, configs.TwoPC, configs.OuAddress)
	for i := 0; i < 5; i++ {
		txn1.AppendRead("MAIN", stmts[0].address, uint64(i))
	}
	_, ok := stmts[0].Manager.PreRead(txn1)
	assert.Equal(t, true, ok)
	CheckVal(t, stmts[0].Manager, []string{"3", "4", "5", "6", "7"})

	txn1.OptList = txn1.OptList[:0]
	for i := 0; i < 5; i++ {
		txn1.AppendUpdate("MAIN", stmts[0].address, uint64(i), storage.WrapTestValue(i+1))
	}
	ok = stmts[0].Manager.PreWrite(txn1)
	assert.Equal(t, true, ok)
	ok = stmts[0].Manager.Abort(txn1)
	assert.Equal(t, true, ok)
	CheckVal(t, stmts[0].Manager, []string{"3", "4", "5", "6", "7"})
	StopServers(stmts)
}

func TestConcurrent(t *testing.T) {
	makeLocal()
	ctx := context.WithValue(context.Background(), "store_list",
		[]string{configs.BenchmarkStorage,
			configs.PostgreSQL,
			configs.MongoDB})
	stmts := TestKit(ctx)

	txn1 := network.NewTXPack(1, stmts[0].address, configs.TwoPC, configs.OuAddress)
	stmts[0].Manager.PreRead(txn1)
	txn2 := network.NewTXPack(2, stmts[0].address, configs.TwoPC, configs.OuAddress)
	stmts[0].Manager.PreRead(txn2)
	CheckVal(t, stmts[0].Manager, []string{"3", "4", "5", "6", "7"})

	txn1.OptList = txn1.OptList[:0]
	txn2.OptList = txn2.OptList[:0]
	for i := 0; i < 2; i++ {
		txn1.AppendUpdate("MAIN", stmts[0].address, uint64(i), storage.WrapTestValue(i+1))
		txn2.AppendUpdate("MAIN", stmts[0].address, uint64(i)+2, storage.WrapTestValue(i+1))
	}
	ch := make(chan bool)
	go func() {
		ok := stmts[0].Manager.PreWrite(txn1)
		configs.Warn(ok, "transaction fails")
		ch <- stmts[0].Manager.Commit(txn1)
	}()
	go func() {
		ok := stmts[0].Manager.PreWrite(txn2)
		configs.Warn(ok, "transaction fails")
		ch <- stmts[0].Manager.Commit(txn2)
	}()
	<-ch
	<-ch

	CheckVal(t, stmts[0].Manager, []string{"1", "2", "1", "2", "7"})
	StopServers(stmts)
}
