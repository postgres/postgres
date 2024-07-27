package participant

import (
	"FC/configs"
	"FC/storage"
	"context"
	"fmt"
	"github.com/magiconair/properties/assert"
	"strconv"
	"testing"
	"time"
)

var address []string

func clearDeadTxn() {
	//cmd := exec.Command("python3", "scripts/recover_txn.py")
	//output, err := cmd.CombinedOutput()
	//if err != nil {
	//	log.Fatalf("Command execution failed: %v, output: %v", err, string(output))
	//}
}

func TestKit(ctx context.Context) []*Context {
	address = make([]string, 0)
	clearDeadTxn()
	for i := 0; i < configs.NumberOfShards; i++ {
		address = append(address, fmt.Sprintf("127.0.0.1:60%02d", i+1))
	}

	stmts := make([]*Context, configs.NumberOfShards)
	ch := make(chan bool)

	for i := 0; i < configs.NumberOfShards; i++ {
		var args = []string{"*", "*", address[i], "*"}
		stmts[i] = &Context{}
		go begin(stmts[i], ch, args[2])
		<-ch
	}

	storeTypes := ctx.Value("store_list").([]string)

	if configs.EnableReplication {
		for i := 0; i < configs.NumberOfShards; i++ {
			for j := 0; j < configs.NumberOfReplicas; j++ {
				// the current nodes contains the data for shard [i] [i-1] ... [i-R+1]
				replica := stmts[i].participants[(i-j+configs.NumberOfShards)%configs.NumberOfShards]
				stmts[i].Manager.Shards[replica] = storage.Testkit(replica, storeTypes[i])
			}
		}
	} else {
		for i := 0; i < configs.NumberOfShards; i++ {
			stmts[i].Manager.Shards[stmts[i].address] = storage.Testkit(stmts[i].address, storeTypes[i])
		}
	}
	return stmts
}

func (ctx *Context) YCSBInit() {
	if configs.EnableReplication {
		panic("not supported yet")
		//for j := 0; j < configs.NumberOfReplicas; j++ {
		//	// the current nodes contains the data for shard [i] [i-1] ... [i-R+1]
		//	replica := ctx.participants[(i-j+configs.NumberOfShards)%configs.NumberOfShards]
		//	ctx.TwoPhaseLockNoWaitManager.Shards[replica] = storage.YCSBStorageKit(replica)
		//}
	} else {
		ctxx := context.WithValue(context.Background(), "store", configs.BenchmarkStorage)
		ctx.Manager.Shards[ctx.address] = storage.YCSBStorageKit(ctxx, ctx.address)
	}
}

func (ctx *Context) TPCInit() {
	if configs.EnableReplication {
		panic("not supported yet")
		//for j := 0; j < configs.NumberOfReplicas; j++ {
		//	// the current nodes contains the data for shard [i] [i-1] ... [i-R+1]
		//	replica := ctx.participants[(i-j+configs.NumberOfShards)%configs.NumberOfShards]
		//	ctx.TwoPhaseLockNoWaitManager.Shards[replica] = storage.YCSBStorageKit(replica)
		//}
	} else {
		ctxx := context.WithValue(context.Background(), "store", configs.BenchmarkStorage)
		ctx.Manager.Shards[ctx.address] = storage.TPCCStorageKit(ctxx, ctx.address)
	}
}

func YCSBParticipantKit(ctx context.Context) []*Context {
	address = make([]string, 0)
	clearDeadTxn()
	for i := 0; i < configs.NumberOfShards; i++ {
		address = append(address, fmt.Sprintf("127.0.0.1:60%02d", i+1))
	}

	stmts := make([]*Context, configs.NumberOfShards)
	ch := make(chan bool)

	for i := 0; i < configs.NumberOfShards; i++ {
		var args = []string{"*", "*", address[i], "*"}
		stmts[i] = &Context{}
		go begin(stmts[i], ch, args[2])
		<-ch
	}

	storeTypes := ctx.Value("store_list").([]string)
	storeDelay := ctx.Value("delay_list").([]time.Duration)

	if configs.EnableReplication {
		for i := 0; i < configs.NumberOfShards; i++ {
			for j := 0; j < configs.NumberOfReplicas; j++ {
				// the current nodes contains the data for shard [i] [i-1] ... [i-R+1]
				replica := stmts[i].participants[(i-j+configs.NumberOfShards)%configs.NumberOfShards]
				subCtx := context.WithValue(ctx, "store", storeTypes[i])
				subCtx = context.WithValue(subCtx, "delay", storeDelay[i])
				stmts[i].Manager.Shards[replica] = storage.YCSBStorageKit(subCtx, replica)
			}
		}
	} else {
		for i := 0; i < configs.NumberOfShards; i++ {
			subCtx := context.WithValue(ctx, "store", storeTypes[i])
			subCtx = context.WithValue(subCtx, "delay", storeDelay[i])
			stmts[i].Manager.Shards[stmts[i].address] = storage.YCSBStorageKit(subCtx, stmts[i].address)
		}
	}
	return stmts
}

func TPCCParticipantKit(ctx context.Context) []*Context {
	address = make([]string, 0)
	clearDeadTxn()
	for i := 0; i < configs.NumberOfShards; i++ {
		address = append(address, fmt.Sprintf("127.0.0.1:60%02d", i+1))
	}

	stmts := make([]*Context, configs.NumberOfShards)
	ch := make(chan bool)

	storeTypes := ctx.Value("store_list").([]string)
	configs.TPrintf("storage types are %v", storeTypes)

	for i := 0; i < configs.NumberOfShards; i++ {
		var args = []string{"*", "*", address[i], "*"}
		stmts[i] = &Context{}
		go begin(stmts[i], ch, args[2])
		<-ch
	}

	if configs.EnableReplication {
		for i := 0; i < configs.NumberOfShards; i++ {
			for j := 0; j < configs.NumberOfReplicas; j++ {
				// the current nodes contains the data for shard [i] [i-1] ... [i-R+1]
				replica := stmts[i].participants[(i-j+configs.NumberOfShards)%configs.NumberOfShards]
				subCtx := context.WithValue(ctx, "store", storeTypes[i])
				stmts[i].Manager.Shards[replica] = storage.TPCCStorageKit(subCtx, replica)
			}
		}
	} else {
		for i := 0; i < configs.NumberOfShards; i++ {
			subCtx := context.WithValue(ctx, "store", storeTypes[i])
			stmts[i].Manager.Shards[stmts[i].address] = storage.TPCCStorageKit(subCtx, stmts[i].address)
		}
	}
	return stmts
}

func LoadIntValue(value interface{}) int {
	//fmt.Println(reflect.TypeOf(value))
	switch v := value.(type) {
	case float64:
		return int(v)
	case uint32:
		return int(v)
	case int32:
		return int(v)
	case uint:
		return int(v)
	case float32:
		return int(v)
	case int:
		return v
	case string:
		val, err := strconv.Atoi(v)
		if err != nil {
			panic(err)
		}
		return val
	default:
		panic("invalid case")
	}
}

func LoadStringValue(value interface{}) string {
	return fmt.Sprintf("%v", value)
}

func CheckVal(t *testing.T, coh *Manager, expected []string) {
	//configs.JPrint("start check")
	for i := 0; i < len(expected); i++ {
		v, ok := coh.Shards[coh.stmt.address].Read("MAIN", uint64(i))
		for !ok {
			v, ok = coh.Shards[coh.stmt.address].Read("MAIN", uint64(i))
		}
		//configs.JPrint(expected[i] + " - " + strconv.Itoa(v.GetAttribute(0).(int)))
		configs.JPrint(v)
		//
		configs.Assert(ok, "value read failed")
		configs.Assert(LoadStringValue(v.GetAttribute(0)) == expected[i],
			fmt.Sprintf("In correct value, want %v, got %v", expected[i], LoadStringValue(v.GetAttribute(0))))
		assert.Equal(t, LoadStringValue(v.GetAttribute(0)), expected[i])
	}
}
