package benchmark

import (
	"FC/configs"
	"FC/network/coordinator"
	"FC/network/participant"
	"FC/storage"
	"FC/utils"
	"context"
	"github.com/pingcap/go-ycsb/pkg/generator"
	"math/rand"
	"strconv"
	"sync/atomic"
	"time"
)

type YCSBStmt struct {
	stat         *utils.Stat
	coordinator  *coordinator.Context
	participants []*participant.Context
	stop         int32
}

type YCSBClient struct {
	md   int
	from *YCSBStmt
	r    *rand.Rand
	zip  *generator.Zipfian
}

func random(min, max int) int {
	return rand.Intn(max-min) + min
}

var letters = []rune("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ")

func randSeq(n int) string {
	b := make([]rune, n)
	for i := range b {
		b[i] = letters[rand.Intn(len(letters))]
	}
	return string(b)
}

func (c *YCSBClient) generateTxnKVPairs(parts []string, TID uint64) []storage.TXOpt {
	kvTransaction := make([]storage.TXOpt, 0)
	var j int
	var val string
	val = randSeq(5)
	md := c.md
	isDistributedTxn := rand.Intn(100) < configs.CrossShardTXNPercentage

	if configs.TransactionLength == 0 {
		// special case used to test transaction varying participants: all participants one operation, used to test the system scalability.
		var key uint64
		for j = 0; j < configs.NumberOfShards; j++ {
			key = uint64(c.zip.Next(c.r))
			/* Access the key from different partitions */
			configs.TPrintf("TXN" + strconv.FormatUint(TID, 10) + ": " + strconv.Itoa(j) + "[" + strconv.FormatUint(key, 10) + "] = " + val)
			isRead := rand.Float64() < configs.ReadPercentage
			if isRead {
				kvTransaction = append(kvTransaction, storage.TXOpt{
					Type:  storage.ReadOpt,
					Shard: parts[j],
					Table: "YCSB_MAIN",
					Key:   key,
				})
			} else {
				kvTransaction = append(kvTransaction, storage.TXOpt{
					Type:  storage.UpdateOpt,
					Shard: parts[j],
					Table: "YCSB_MAIN",
					Key:   key,
					Value: storage.WrapYCSBTestValue(val + strconv.FormatUint(TID, 10)),
				})
			}
		}
	} else {
		var key uint64
		for i := 0; i < configs.TransactionLength; i++ {
			/* Code for contentious key selection */
			j = random(0, configs.ShardsPerTransaction)
			if i < configs.ShardsPerTransaction && isDistributedTxn {
				/* Ensure txn spans all partitions */
				j = (i + md) % configs.NumberOfShards
				val = configs.ZeroValue
			} else if !isDistributedTxn {
				j = md % configs.NumberOfShards
			} else {
				j = (j + md) % configs.NumberOfShards
			}
			key = uint64(c.zip.Next(c.r))
			/* Access the key from different partitions */
			configs.TPrintf("TXN" + strconv.FormatUint(TID, 10) + ": " + strconv.Itoa(j) + "[" + strconv.FormatUint(key, 10) + "] = " + val)

			isRead := rand.Float64() < configs.ReadPercentage
			if isRead {
				kvTransaction = append(kvTransaction, storage.TXOpt{
					Type:  storage.ReadOpt,
					Shard: parts[j],
					Table: "YCSB_MAIN",
					Key:   key,
				})
			} else {
				kvTransaction = append(kvTransaction, storage.TXOpt{
					Type:  storage.UpdateOpt,
					Shard: parts[j],
					Table: "YCSB_MAIN",
					Key:   key,
					Value: storage.WrapYCSBTestValue(val),
				})
			}
		}
	}
	return kvTransaction
}

func (c *YCSBClient) performTransactions(TID uint64, participants []string, txn *coordinator.TX, stats *utils.Stat) (bool, *coordinator.TX) {
	defer configs.TimeTrack(time.Now(), "performTransactions", TID)
	if txn == nil {
		kvData := c.generateTxnKVPairs(participants, TID)
		exist := make(map[string]bool)
		parts := make([]string, 0)
		for _, v := range kvData {
			sd := v.Shard
			if exist[sd] == false {
				exist[sd] = true
				parts = append(parts, sd)
			}
		}
		txn = coordinator.NewTX(TID, parts, c.from.coordinator.Manager)
		txn.OptList = kvData
	} else {
		txn.TxnID = TID
	}
	info := utils.NewInfo(len(txn.Participants))
	configs.DPrintf("TXN%v: Start on client %v", TID, c.md)
	c.from.coordinator.Manager.SubmitTxn(txn, configs.SelectedACP, info)
	stats.Append(info)
	if info.IsCommit {
		configs.DPrintf("TXN%v: Commit on client %v", TID, c.md)
	} else {
		configs.DPrintf("TXN%v: Abort on client %v", TID, c.md)
	}
	return info.IsCommit, txn
}

func (stmt *YCSBStmt) Stopped() bool {
	return atomic.LoadInt32(&stmt.stop) != 0
}

func (stmt *YCSBStmt) startYCSBClient(seed int, md int) {
	client := YCSBClient{md: md, from: stmt}

	client.r = rand.New(rand.NewSource(int64(seed)*11 + 31))
	client.zip = generator.NewZipfianWithRange(0, int64(configs.NumberOfRecordsPerShard-2), configs.YCSBDataSkewness)
	for !stmt.Stopped() {
		TID := utils.GetTxnID()
		client.performTransactions(TID, configs.OuAddress, nil, stmt.stat)
	}
}

func (stmt *YCSBStmt) Stop() {
	stmt.coordinator.Close()
	atomic.StoreInt32(&stmt.stop, 1)
	if stmt.participants == nil {
		return
	}
	for _, v := range stmt.participants {
		v.Close()
	}
}

func (stmt *YCSBStmt) YCSBTest() {
	ctx := context.WithValue(context.Background(), "store_list",
		[]string{configs.PostgreSQL,
			configs.PostgreSQL,
			configs.PostgreSQL})
	if configs.NumberOfShards == 1 {
		ctx = context.WithValue(ctx, "delay_list", []time.Duration{
			// delay to the blocking window.
			configs.InjectDelay,
			configs.InjectDelay,
			configs.InjectDelay,
		})
	} else {
		ctx = context.WithValue(ctx, "delay_list", []time.Duration{
			// delay to the blocking window.
			0,
			configs.InjectDelay,
			configs.InjectDelay,
		})
	}
	if configs.SelectedACP == configs.GPAC {
		configs.EnableReplication = true
	}
	if configs.LocalTest {
		stmt.coordinator, stmt.participants = coordinator.YCSBTestKit(ctx)
	} else {
		stmt.coordinator = coordinator.NormalKit(configs.CoordinatorServerAddress)
		stmt.participants = nil
	}
	stmt.stat = utils.NewStat()
	rand.Seed(1234)
	for i := 0; i < configs.ClientRoutineNumber; i++ {
		go stmt.startYCSBClient(i*11+13, i)
	}
	configs.TPrintf("All clients Started")
	if configs.TimeElapsedTest {
		stmt.stat.Clear()
		for i := time.Duration(0); i < 10*time.Second; i += time.Millisecond * 10 {
			time.Sleep(10 * time.Millisecond)
			stmt.stat.Log()
			stmt.stat.Clear()
		}
	} else {
		time.Sleep(configs.WarmUpTime)
		//stmt.stat.Log()
		stmt.stat.Clear()
		time.Sleep(configs.RunTestInterval * time.Second)
		stmt.stat.Log()
		//stmt.stat.Range()
		stmt.stat.Clear()
	}
}
