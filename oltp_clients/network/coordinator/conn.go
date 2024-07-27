package coordinator

import (
	"FC/configs"
	"FC/network"
	"bufio"
	"github.com/goccy/go-json"
	"io"
	"net"
	"strconv"
	"sync"
	"time"
)

type Commu struct {
	done     chan bool
	listener net.Listener
	stmt     *Context
	connMap  *sync.Map
	sem      chan struct{}
}

func NewConns(stmt *Context, address string) *Commu {
	res := &Commu{stmt: stmt}
	res.connMap = &sync.Map{}
	res.done = make(chan bool, 1)
	tcpAddr, err := net.ResolveTCPAddr("tcp4", address)
	configs.CheckError(err)
	res.listener, err = net.ListenTCP("tcp", tcpAddr)
	configs.CheckError(err)
	return res
}

func (c *Commu) Run() {
	c.sem = make(chan struct{}, configs.MaxConnectionHandler)
	for {
		configs.TPrintf("running another message round")
		conn, err := c.listener.Accept()
		if err != nil {
			configs.TPrintf("accepted a package: %s", err.Error())
		} else {
			configs.TPrintf("accepted a package: succeed")
		}
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

func (c *Commu) Close() {
	c.done <- true
	c.connMap.Range(func(key, value interface{}) bool {
		configs.CheckError(value.(net.Conn).Close())
		return true
	})
	configs.CheckError(c.listener.Close())
}

func (c *Commu) handleRequest(conn net.Conn) {
	defer conn.Close()
	reader := bufio.NewReader(conn)
	for {
		//configs.JPrint(c)
		data, err := reader.ReadString('\n')
		//configs.JPrint(c)
		if err == io.EOF {
			break
		}
		configs.CheckError(err)
		go c.stmt.handleRequestType([]byte(data))
	}
}

func (c *Commu) connHandler(conn net.Conn) {
	ch := make(chan []byte)
	eCh := make(chan error)
	reader := bufio.NewReader(conn)
	go func(ch chan []byte, eCh chan error) {
		for {
			data, err := reader.ReadString('\n')
			if err != nil {
				eCh <- err
				return
			}
			ch <- []byte(data)
		}
	}(ch, eCh)

	ticker := time.Tick(time.Second)
	for {
		select {
		case data := <-ch:
			c.stmt.handleRequestType(data)
		case err := <-eCh:
			configs.TPrintf(err.Error())
		case <-ticker:
		}
	}
}

func (c *Commu) sendMsg(to string, msg []byte) {
	var conn net.Conn
	//configs.JPrint(to)
	if cur, ok := c.connMap.Load(to); !ok {
		tcpAddr, err := net.ResolveTCPAddr("tcp4", to)
		configs.CheckError(err)
		newConn, err := net.DialTCP("tcp", nil, tcpAddr)
		configs.CheckError(err)
		fin, _ := c.connMap.LoadOrStore(to, newConn)
		conn = fin.(net.Conn)
	} else {
		conn = cur.(net.Conn)
	}
	msg = append(msg, "\n"...)
	err := conn.SetWriteDeadline(time.Now().Add(1 * time.Second))
	if err != nil {
		configs.Warn(false, err.Error())
	}
	_, err = conn.Write(msg)
	if err != nil {
		configs.Warn(false, err.Error())
	}
}

func (stmt *Context) handleRequestType(requestBytes []byte) {
	var request network.Response4Coordinator
	err := json.Unmarshal(requestBytes, &request)
	configs.CheckError(err)
	//configs.JPrint(request)
	if request.ACK {
		configs.TxnPrint(request.TID, "CA Got message with Mark "+request.Mark+" sign: true")
	} else {
		configs.TxnPrint(request.TID, "CA Got message with Mark "+request.Mark+" sign: false")
	}
	tx := stmt.Manager.ignoreIfNotExistTxnHandler(request.TID)
	if tx != nil {
		tx.handleResponse(&request)
	} else {
		configs.TPrintf("TXN" + strconv.FormatUint(request.TID, 10) + ": received a message without handler")
	}
}

//if request.Mark == configs.ReadUnsuccessful {
//stmt.Manager.handlePreRead(request.TID, &request.Read, false)
//} else if request.Mark == configs.ReadSuccess {
//stmt.Manager.handlePreRead(request.TID, &request.Read, true)
//} else if request.Mark == configs.Finished || request.Mark == configs.PreCommitACK ||
//request.Mark == configs.PreWriteACK {
//stmt.Manager.handleACK(request.TID, &request)
//} else if request.Mark == configs.FCResults {
//stmt.Manager.handleFC(request.TID, &request.Res)
//}
