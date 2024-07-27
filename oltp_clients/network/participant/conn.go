package participant

import (
	"FC/configs"
	"FC/network"
	"bufio"
	"github.com/goccy/go-json"
	"io"
	"math"
	"math/rand"
	"net"
	"strconv"
	"sync"
	"time"
)

type Comm struct {
	done     chan bool
	listener net.Listener
	stmt     *Context
	connMap  *sync.Map
	sem      chan struct{}
}

func NewConns(stmt *Context, address string) *Comm {
	res := &Comm{stmt: stmt}
	res.connMap = &sync.Map{}
	res.done = make(chan bool, 1)
	tcpAddr, err := net.ResolveTCPAddr("tcp4", address)
	configs.CheckError(err)
	res.listener, err = net.ListenTCP("tcp", tcpAddr)
	configs.CheckError(err)
	return res
}

func (c *Comm) Run() {
	c.sem = make(chan struct{}, configs.MaxConnectionHandler)
	for {
		conn, err := c.listener.Accept()
		if err != nil {
			select {
			case <-c.done:
				return
			default:
				configs.CheckError(err)
			}
		}
		c.sem <- struct{}{}
		go func() {
			defer func() {
				<-c.sem
			}()
			c.handleRequest(conn)
		}()
	}
}

func (c *Comm) handleRequest(conn net.Conn) {
	defer conn.Close()
	reader := bufio.NewReader(conn)
	for {
		data, err := reader.ReadString('\n')
		if err == io.EOF {
			break
		}
		configs.CheckError(err)
		go c.stmt.handleRequestType([]byte(data))
	}
}

func (c *Comm) Stop() {
	c.done <- true
	c.connMap.Range(func(key, value interface{}) bool {
		configs.CheckError(value.(net.Conn).Close())
		return true
	})
	configs.CheckError(c.listener.Close())
}

func (c *Comm) sendMsg(to string, msg []byte) {
	var conn net.Conn
	if cur, ok := c.connMap.Load(to); !ok {
		tcpAddr, err := net.ResolveTCPAddr("tcp4", to)
		configs.CheckError(err)
		newConn, err := net.DialTCP("tcp", nil, tcpAddr)
		fin, _ := c.connMap.LoadOrStore(to, newConn)
		//if !ok {
		//	return
		//}
		conn = fin.(net.Conn)
	} else {
		conn = cur.(net.Conn)
	}
	msg = append(msg, "\n"...)
	//configs.JPrint(msg)
	if conn == nil {
		panic("the connection becomes empty")
		return
	}
	err := conn.SetWriteDeadline(time.Now().Add(1 * time.Second))
	if err != nil {
		configs.Warn(false, err.Error())
	}
	_, err = conn.Write(msg)
	if err != nil {
		configs.Warn(false, err.Error())
	}
}

func (ctx *Context) handleRequestType(requestBytes []byte) {
	/* Checks the kind of request sent to coordinator. Calls relevant functions based
	on the request. */
	if ctx.Manager.isBroken() {
		ctx.queueLatch.Lock()
		ctx.msgQueue = append(ctx.msgQueue, requestBytes)
		ctx.queueLatch.Unlock()
		// To simulate the crash failure,
		// we delay all messages arrived to the current node until the node recovers.
		configs.LPrintf("Message get lost due to crash failure on node " + ctx.address)
		return
	}
	// we receive the messages for crashed node because currently we do not have a recovery protocol.
	// In FCFF, these messages will get logged on the coordinator and sent to the participants when they recover.
	var request network.PaGossip
	err := json.Unmarshal(requestBytes, &request)
	configs.CheckError(err)
	if request.Txn != nil {
		configs.DPrintf("TXN" + strconv.FormatUint(request.Txn.TxnID, 10) + ": " + "Pending message for " + ctx.address + " with Mark " + request.Mark)
	} else {
		configs.DPrintf("TXN" + strconv.FormatUint(request.Vt.TID, 10) + ": " + "Pending message for " + ctx.address + " with Mark " + request.Mark)
	}
	txn := request.Txn
	if request.Mark == configs.PreRead {
		/* A new Txn started involving this replica. For all protocols */
		res, ok := ctx.Manager.PreRead(txn)
		if !ok {
			ctx.Manager.sendBackCA(txn.TxnID, txn.ShardID, configs.ReadUnsuccessful, res, request.BeginTime)
		} else {
			ctx.Manager.sendBackCA(txn.TxnID, txn.ShardID, configs.ReadSuccess, res, request.BeginTime)
		}
	} else if request.Mark == configs.PreWrite {
		// For 2PC, 3PC, single shard, EasyCommit, C-PAC.
		res := ctx.Manager.PreWrite(txn)
		ctx.Manager.sendBackCA(txn.TxnID, txn.ShardID, configs.PreWriteACK, res, request.BeginTime)
	} else if request.Mark == configs.Commit {
		// For all protocols
		configs.Assert(ctx.Manager.Commit(txn), "The commit is not executed")
		if request.Txn.Protocol != configs.EasyCommit {
			ctx.Manager.sendBackCA(txn.TxnID, txn.ShardID, configs.Finished, true, request.BeginTime)
		}
	} else if request.Mark == configs.Abort {
		// For all protocols
		configs.Assert(ctx.Manager.Abort(txn), "The abort is not executed")
		if request.Txn.Protocol != configs.EasyCommit {
			ctx.Manager.sendBackCA(txn.TxnID, txn.ShardID, configs.Finished, false, request.BeginTime)
		}
	} else if request.Mark == configs.FCProposed {
		// For FCFF only.
		res := ctx.Manager.Propose(txn, request.BeginTime)
		configs.Assert(res != nil, "Nil ptr encountered for result")
		ctx.Manager.sendBackCA(txn.TxnID, txn.ShardID, configs.FCResults, *res, request.BeginTime)
	} else if request.Mark == configs.PreCommit {
		// For 3PC only.
		res := ctx.Manager.PreCommit(txn)
		if txn.Protocol == configs.GPAC && !res {
			// do not reply anything.
			return
		}
		ctx.Manager.sendBackCA(txn.TxnID, txn.ShardID, configs.PreCommitACK, res, request.BeginTime)
	} else if request.Mark == configs.FCGossipVotes {
		if ctx.Manager.isDisrupted() {
			// the network failure can be simulated by just adding a delay in message handling.
			time.Sleep(time.Duration(math.Max(0, float64(configs.DelayStdDev)*rand.NormFloat64())))
		}
		// For FC-FF, EasyCommit.
		if request.Vt.Protocol == configs.EasyCommit {
			ctx.Manager.handleVote(request.Vt, 0)
		} else {
			ctx.Manager.handleVote(request.Vt, time.Since(request.BeginTime))
		}
	}
}
