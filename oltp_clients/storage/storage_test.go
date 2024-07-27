package storage

import (
	"FC/configs"
	"fmt"
	"github.com/magiconair/properties/assert"
	"math/rand"
	"sync/atomic"
	"testing"
	"time"
)

func TestNoContentionWrite(t *testing.T) {
	s := Testkit("id", configs.BenchmarkStorage)
	st := time.Now()
	for i := 0; i < 100000; i++ {
		ok := s.Update("MAIN", uint64(rand.Intn(1000)+1), s.GenTestValue())
		assert.Equal(t, ok, true)
	}
	fmt.Println("No contention write/second = ", 100000.0/float64(time.Since(st).Seconds()))
}

func TestNoContentionRead(t *testing.T) {
	s := Testkit("id", configs.BenchmarkStorage)
	st := time.Now()
	for i := 0; i < 100000; i++ {
		key := uint64(rand.Intn(1000) + 1)
		v, ok := s.Read("MAIN", key)
		assert.Equal(t, ok, true)
		assert.Equal(t, int(key+3), v.GetAttribute(0).(int))
	}
	fmt.Println("No contention read/second = ", 100000.0/float64(time.Since(st).Seconds()))
}

func TestW4R(t *testing.T) {
	s := Testkit("id", configs.BenchmarkStorage)
	go func() {
		for i := 0; i < 100000; i++ {
			s.Read("MAIN", uint64(rand.Intn(100)+1))
		}
	}()
	go func() {
		for i := 0; i < 1000000; i++ {
			s.Read("MAIN", uint64(rand.Intn(100)+1))
		}
	}()
	st := time.Now()
	for i := 0; i < 100000; i++ {
		s.Update("MAIN", uint64(rand.Intn(1000)+1), s.GenTestValue())
	}
	fmt.Println("Write/second with two thread accessing = ", 100000.0/float64(time.Since(st).Seconds()))
}

func TxnAccess(shard *Shard, n, rang uint64, readPr float64, txnID uint32) bool { // Solved because of random function!!!!!
	shard.Begin(txnID)
	for i := uint64(0); i < n; i++ {
		if rand.Float64() < readPr {
			_, ok := shard.ReadTxn("MAIN", txnID, rand.Uint64()%rang)
			if !ok {
				shard.RollBack(txnID)
				return false
			}
		} else {
			if !shard.UpdateTxn("MAIN", txnID, rand.Uint64()%rang, shard.GenTestValue()) {
				shard.RollBack(txnID)
				return false
			}
		}
	}
	shard.Commit(txnID)
	return true
}

func TestTxNNoContention(t *testing.T) {
	s := Testkit("id", configs.PostgreSQL)
	st := time.Now()
	suc := 0
	for i := uint32(0); i < 1000; i++ {
		if TxnAccess(s, 5, 200, 0.5, i) {
			suc++
		}
	}
	bas := float64(time.Since(st).Seconds())
	fmt.Println("txn/second without contention", float64(suc)/bas)
}

func TestTxNConcurrent(t *testing.T) {
	s := Testkit("id", configs.PostgreSQL)
	var latencySum int64 = 0
	for con := 1; con < 32; con *= 2 {
		st := time.Now()
		suc := int32(0)
		ch := make(chan bool, con)
		for c := uint32(0); c < uint32(con); c++ {
			go func(done chan bool, thrID uint32) {
				for i := 0; i < configs.SpeedTestBatchPerThread; i++ {
					txnBeginTime := time.Now()
					tid := uint32(i + configs.SpeedTestBatchPerThread*int(thrID))
					if TxnAccess(s, 5, uint64(configs.NumberOfRecordsPerShard-1), 0.5, tid) {
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
		fmt.Printf("with %v concurrent processes, %.2f local transactions executed in one second, %.2f success, %.2f (ms) average latency\n",
			con, float64(configs.SpeedTestBatchPerThread)*float64(con)/bas, float64(suc)/bas, totTime.Seconds()*1000/float64(suc))
	}
}

func TestSQLConn(t *testing.T) {
	s := Testkit("id", configs.PostgreSQL)
	var latencySum int64 = 0
	for con := 1; con < 32; con *= 2 {
		st := time.Now()
		suc := int32(0)
		ch := make(chan bool, con)
		for c := uint32(0); c < uint32(con); c++ {
			go func(done chan bool, thrID uint32) {
				for i := 0; i < configs.SpeedTestBatchPerThread; i++ {
					txnBeginTime := time.Now()
					tid := uint32(i + configs.SpeedTestBatchPerThread*int(thrID))
					if TxnAccess(s, 4, uint64(configs.NumberOfRecordsPerShard-1), 0.5, tid) {
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
		fmt.Printf("with %v concurrent threads, %.2f local transactions executed in one second, %.2f success, %.2f (ms) average latency\n",
			con, float64(configs.SpeedTestBatchPerThread)*float64(con)/bas, float64(suc)/bas, totTime.Seconds()*1000/float64(suc))
	}
}

func TestMongoDBConn(t *testing.T) {
	s := Testkit("test_id", configs.MongoDB)
	var latencySum int64 = 0
	for con := 16; con < 32; con *= 2 {
		st := time.Now()
		suc := int32(0)
		ch := make(chan bool, con)
		for c := uint32(0); c < uint32(con); c++ {
			go func(done chan bool, thrID uint32) {
				for i := 0; i < configs.SpeedTestBatchPerThread; i++ {
					txnBeginTime := time.Now()
					tid := uint32(i + configs.SpeedTestBatchPerThread*int(thrID))
					if TxnAccess(s, 5, uint64(configs.NumberOfRecordsPerShard-1), 0.5, tid) {
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
		fmt.Printf("with %v concurrent threads, %.2f local transactions executed in one second, %.2f success, %.2f (ms) average latency\n",
			con, float64(configs.SpeedTestBatchPerThread)*float64(con)/bas, float64(suc)/bas, totTime.Seconds()*1000/float64(suc))
	}
}
