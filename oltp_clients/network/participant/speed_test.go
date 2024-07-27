package participant

import (
	"FC/configs"
	"FC/network"
	"FC/storage"
	"context"
	"fmt"
	"math/rand"
	"sync/atomic"
	"testing"
	"time"
)

func txnAccess(stmt *Context, n, rang int, txnID uint64, readPer float64) bool {
	configs.TimeTrack(time.Now(), "txnAccess in participant", txnID)
	tx := network.NewTXPack(txnID, stmt.address, configs.NoACP, []string{stmt.address})
	for i := 0; i < n; i++ {
		if rand.Float64() > readPer {
			tx.AppendRead("MAIN", stmt.address, uint64(rand.Intn(rang)+1))
		} else {
			tx.AppendUpdate("MAIN", stmt.address, uint64(rand.Intn(rang)+1), storage.WrapTestValue(rand.Intn(10000)))
		}
	}
	return stmt.Manager.PreWrite(tx)
}

func TestParticipantSideLocalTxN(t *testing.T) {
	makeLocal()
	ctx := context.WithValue(context.Background(), "store_list",
		[]string{configs.BenchmarkStorage,
			configs.BenchmarkStorage,
			configs.BenchmarkStorage})
	stmts := TestKit(ctx)
	configs.StoredProcedure = true
	rand.Seed(233)
	var latencySum int64 = 0
	for con := 1; con < 16; con *= 2 {
		st := time.Now()
		suc := int32(0)
		ch := make(chan bool, con)
		for c := 0; c < con; c++ {
			go func(done chan bool, pos int) {
				for i := 0; i < configs.SpeedTestBatchPerThread; i++ {
					txnBeginTime := time.Now()
					opt := rand.Intn(3)
					if txnAccess(stmts[opt], 5, configs.NumberOfRecordsPerShard-1, uint64(i+configs.SpeedTestBatchPerThread*pos), 0.5) {
						atomic.AddInt64(&latencySum, int64(time.Since(txnBeginTime)))
						atomic.AddInt32(&suc, 1)
					}
				}
				done <- true
			}(ch, c)
		}
		for i := 0; i < con; i++ {
			<-ch
		}
		totTime := time.Duration(latencySum)
		bas := time.Since(st).Seconds()
		fmt.Printf("with %v concurrent threads, %.2f local transactions executed in one second, "+
			"%.2f success, %.2f (ms) average latency\n",
			con,
			float64(configs.SpeedTestBatchPerThread)*float64(con)/bas,
			float64(suc)/bas, totTime.Seconds()*1000/float64(suc))
	}
}
