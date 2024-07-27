package benchmark

import (
	"FC/configs"
	"FC/network/coordinator"
	"FC/network/participant"
	"FC/storage"
	"FC/utils"
	"context"
	set "github.com/deckarep/golang-set"
	"math/rand"
	"strconv"
	"sync/atomic"
	"time"
)

type TPCClient struct {
	clientID    int
	needStock   set.Set
	payed       set.Set
	allOrderIDs set.Set
	pop         int
	from        *TPCStmt
}

func NewTPCClient(id int) *TPCClient {
	c := &TPCClient{}
	c.clientID = id
	c.needStock = set.NewSet()
	c.payed = set.NewSet() // <= 1000
	c.allOrderIDs = set.NewSet()
	return c
}

const (
	Newed         = 0
	Payed         = 1
	Delivered     = 2
	NWareHouse    = 3
	PayedSize     = 100
	NeedStockSize = 20
	AllOrderSize  = 2
)

func (stmt *TPCStmt) Init() {
	ctx := context.WithValue(context.Background(), "store_list",
		[]string{configs.BenchmarkStorage,
			configs.MongoDB,
			configs.MongoDB})
	if configs.LocalTest {
		stmt.coordinator, stmt.participants = coordinator.TPCCTestKit(ctx)
	} else {
		stmt.coordinator = coordinator.NormalKit(configs.CoordinatorServerAddress)
		stmt.participants = nil
	}
	stmt.protocol = configs.SelectedACP
}

type TPCOrderLine struct {
	Item      int
	Count     int
	Warehouse int
}

type TPCOrder struct {
	Order int
	Ware  int
	//Customer int
	District   int
	HAmount    float64
	Items      []*TPCOrderLine
	MainNode   int
	StockParts []int
}

func getWarehouseFromShard(shardID int) int {
	base := shardID * configs.NumberOfWareHousePerShards
	return base + random(0, configs.NumberOfWareHousePerShards)
}

func getWarehouseID(ware int) int {
	return ware % configs.NumberOfWareHousePerShards
}

func (stmt *TPCStmt) GetOrder() *TPCOrder {
	baseWh := random(0, configs.NumberOfShards*configs.NumberOfWareHousePerShards)
	res := &TPCOrder{
		Order:      random(0, 1000),
		Ware:       baseWh,
		District:   random(0, 10),
		HAmount:    10,
		Items:      make([]*TPCOrderLine, 0),
		MainNode:   whToShard(baseWh),
		StockParts: make([]int, 0),
	}
	res.StockParts = append(res.StockParts, res.MainNode)
	for i := 0; i < 5; i++ {
		cur := TPCOrderLine{
			Item:      random(0, 10000),
			Count:     5,
			Warehouse: baseWh,
		}
		res.Items = append(res.Items, &cur)
	}
	isDistributed := rand.Float64() < 0.15
	if isDistributed {
		shard := (res.MainNode + 1) % configs.NumberOfShards
		res.StockParts = append(res.StockParts, shard)
		for i := 0; i < 5; i++ {
			shard = (res.MainNode + i%2) % configs.NumberOfShards
			res.Items[i].Warehouse = getWarehouseFromShard(shard)
		}
	}
	return res
}

//func (c *TPCStmt) checkAndLoadStock(client *startTPCClient, order *TPCOrder, res map[string]string) bool {
//	for _, v := range order.Items {
//		key := utils.TransTableItem(v.Warehouse, v.Item)
//		val, err := strconv.Atoi(res[configs.Hash(configs.OuAddress[v.Warehouse], key)])
//		if err != nil {
//			panic(err)
//		}
//		if val < 10 {
//			if len(client.needStock.ToSlice()) < NeedStockSize {
//				client.needStock.Add(v.Item*NWareHouse + v.Warehouse)
//			}
//		}
//	}
//	for _, v := range order.Items {
//		val, err := strconv.Atoi(res[configs.Hash(configs.OuAddress[v.Warehouse], key)])
//		if err != nil {
//			panic(err)
//		}
//		if val < 5 {
//			return true
//		}
//	}
//	return true
//}

func (stmt *TPCStmt) payment(client *TPCClient, order *TPCOrder, parts []string) bool {
	configs.TPrintf("Handle payment begins from Client:" + strconv.Itoa(client.clientID))
	noTID := utils.GetTxnID()
	address := configs.OuAddress[order.MainNode]
	paRead := coordinator.NewTX(noTID, []string{address}, stmt.coordinator.Manager)
	whKey := storage.GetTableKey(configs.WAREHOUSE, getWarehouseID(order.Ware), 0, 0, 0, 0)
	paRead.AddRead(configs.WAREHOUSE, address, uint64(whKey))
	disKey := storage.GetTableKey(configs.DISTRICT, getWarehouseID(order.Ware), order.District, 0, 0, 0)
	paRead.AddRead(configs.DISTRICT, address, uint64(disKey))
	res, ok := stmt.coordinator.Manager.PreRead(paRead)
	if !ok {
		return false
	}
	paWrite := coordinator.NewTX(noTID, parts, stmt.coordinator.Manager)
	key := paRead.OptList[0].GetKey()
	rowWh := res[key]
	if rowWh == nil {
		rowWh = storage.NewRowDataWithLength(8)
		rowWh.SetAttribute(configs.WhYTD, 0.001)
	}
	rowWh.SetAttribute(configs.WhYTD, rowWh.GetAttribute(configs.WhYTD).(float64)+order.HAmount)
	paWrite.AddUpdate(configs.WAREHOUSE, address, uint64(whKey), rowWh)
	key = paRead.OptList[1].GetKey()
	rowD := res[key]
	if rowD == nil {
		rowD = storage.NewRowDataWithLength(10)
		rowD.SetAttribute(configs.DYTD, 0.001)
	}

	rowD.SetAttribute(configs.DYTD, rowD.GetAttribute(configs.DYTD).(float64)+order.HAmount)
	paWrite.AddUpdate(configs.DISTRICT, address, uint64(disKey), rowD)
	ok = stmt.coordinator.Manager.SubmitTxn(paWrite, stmt.protocol, nil)
	for _, v := range order.Items {
		if len(client.payed.ToSlice()) < PayedSize {
			client.payed.Add(order.Order*NWareHouse + v.Warehouse)
		}
	}
	configs.TPrintf("Handle payment ends from Client:" + strconv.Itoa(client.clientID))
	return ok
}

func ToRow(value interface{}, len int) *storage.RowData {
	res := storage.NewRowDataWithLength(len)
	res.Value[0] = value
	return res
}

func whToShard(wh int) int {
	return wh / configs.NumberOfWareHousePerShards
}

func (stmt *TPCStmt) newOrder(client *TPCClient, order *TPCOrder, parts []string, stats *utils.Stat) bool {
	configs.TPrintf("Handle new-order begins from Client:" + strconv.Itoa(client.clientID))
	noTID := utils.GetTxnID()
	configs.TPrintf("new-order ts = %d Client:"+strconv.Itoa(client.clientID), noTID)
	defer configs.TimeTrack(time.Now(), "for new-order", noTID)
	noRead := coordinator.NewTX(noTID, parts, stmt.coordinator.Manager)
	for _, v := range order.Items {
		stockKey := storage.GetTableKey(configs.STOCK, getWarehouseID(v.Warehouse), 0, 0, 0, v.Item)
		noRead.AddRead(configs.STOCK, configs.OuAddress[whToShard(v.Warehouse)], uint64(stockKey))
	}
	for _, v := range order.Items {
		whKey := storage.GetTableKey(configs.WAREHOUSE, getWarehouseID(v.Warehouse), 0, 0, 0, 0)
		noRead.AddRead(configs.WAREHOUSE, configs.OuAddress[whToShard(v.Warehouse)], uint64(whKey))
	}
	val, ok := stmt.coordinator.Manager.PreRead(noRead)
	configs.TPrintf("new-order read done from client: " + strconv.Itoa(client.clientID))
	if !ok {
		configs.TxnPrint(noTID, "failed for blocked pre-read")
		return false
	}
	noWrite := coordinator.NewTX(noTID, parts, stmt.coordinator.Manager)
	for i, v := range order.Items {
		oldStock := val[noRead.OptList[i].GetKey()]
		if oldStock == nil {
			oldStock = storage.NewRowDataWithLength(8)
			oldStock.SetAttribute(configs.SQuantity, 100)
		}
		val, ok := oldStock.GetAttribute(configs.SQuantity).(int)
		if !ok {
			val = int(oldStock.GetAttribute(configs.SQuantity).(float64))
		}
		oldStock.SetAttribute(configs.SQuantity, val-5)
		noWrite.AddUpdate(configs.STOCK, configs.OuAddress[whToShard(v.Warehouse)], noRead.OptList[i].Key, oldStock)
	}
	orderKey := storage.GetTableKey(configs.ORDER, getWarehouseID(order.Ware), 0, 0, order.Order, 0)
	noWrite.AddUpdate(configs.ORDER, configs.OuAddress[order.MainNode], uint64(orderKey), ToRow(Newed, 5))
	info := utils.NewInfo(len(noWrite.Participants))
	ok = stmt.coordinator.Manager.SubmitTxn(noWrite, stmt.protocol, info)
	stats.Append(info) // old TS
	if !ok {
		configs.TxnPrint(noTID, "Failed for commit")
	}
	if len(client.allOrderIDs.ToSlice()) < AllOrderSize {
		client.allOrderIDs.Add(order.Ware + order.Order*NWareHouse)
	}
	//c.checkAndLoadStock(client, order, val)
	configs.TPrintf("Handle newOrder begins from Client:" + strconv.Itoa(client.clientID))
	return ok
}

// HandleOrder handle an Order from tpcc-generator
func (stmt *TPCStmt) HandleOrder(client *TPCClient, order *TPCOrder, stats *utils.Stat) bool {
	parts := make([]string, 0)
	for _, v := range order.StockParts {
		parts = append(parts, configs.OuAddress[v])
	}
	/// NewOrderTxn
	if stmt.newOrder(client, order, parts, stats) {
		configs.TPrintf("NewOrder Success")
	} else {
		configs.TPrintf("NewOrder Failed")
		return false
	}
	stmt.payment(client, order, parts)
	return true
}

//func (c *TPCStmt) stockLevel(order *TPCOrder, parts []string) bool {
//	slTID := utils.GetTxnID()
//	slRead := coordinator.NewTX(slTID, parts, c.coordinator.TwoPhaseLockNoWaitManager)
//	for _, v := range order.Items {
//		slRead.AddRead("STOCK", configs.OuAddress[v.Warehouse], utils.TransTableItem(v.Warehouse, v.Item))
//	}
//	val, ok := c.coordinator.TwoPhaseLockNoWaitManager.PreRead(slRead)
//	if !ok {
//		configs.TPrintf("Failed for Warehouse read")
//		return false
//	}
//	slWrite := coordinator.NewTX(slTID, parts, c.coordinator.TwoPhaseLockNoWaitManager)
//	for i, v := range order.Items {
//		key := utils.TransTableItem(v.Warehouse, v.Item)
//		vc := val[slRead.OptList[i].GetKey()]
//		curStock := vc.GetAttribute(configs.SQuantity).(int)
//		vc.SetAttribute(configs.SQuantity, curStock+100)
//		slWrite.AddUpdate("stock", configs.OuAddress[v.Warehouse], key, vc)
//	}
//	ok = c.coordinator.TwoPhaseLockNoWaitManager.SubmitTxn(slWrite, c.protocol, nil)
//	return ok
//}
//
//// HandleStockLevel introduce Items with saved Warehouse requests.
//func (c *TPCStmt) HandleStockLevel(client *startTPCClient) {
//	configs.TPrintf("Handle stock begins from Client:" + strconv.Itoa(client.clientID))
//	exist := make(map[int]bool)
//	parts := make([]string, 0)
//	tmp := &TPCOrder{Items: make([]*TPCOrderLine, 0)}
//	for _, v := range client.needStock.ToSlice() {
//		ware := v.(int) % NWareHouse
//		item := v.(int) / NWareHouse
//		if !exist[ware] {
//			exist[ware] = true
//			parts = append(parts, configs.OuAddress[ware])
//		}
//		tmp.Items = append(tmp.Items, &TPCOrderLine{
//			Warehouse: ware,
//			Item:  item,
//		})
//	}
//	configs.TPrintf("From Client:" + strconv.Itoa(client.clientID))
//	c.stockLevel(tmp, parts)
//	configs.TPrintf("Handle stock ends from Client:" + strconv.Itoa(client.clientID))
//}

func (stmt *TPCStmt) delivery(order *TPCOrder, parts []string) bool {
	deTID := utils.GetTxnID()
	deWrite := coordinator.NewTX(deTID, []string{configs.OuAddress[order.MainNode]}, stmt.coordinator.Manager)
	orderKey := storage.GetTableKey(configs.ORDER, getWarehouseID(order.Ware), 0, 0, order.Order, 0)
	deWrite.AddUpdate(configs.ORDER, configs.OuAddress[order.MainNode], uint64(orderKey), ToRow(Delivered, 5))
	ok := stmt.coordinator.Manager.SubmitTxn(deWrite, stmt.protocol, nil)
	return ok
}

// HandleDelivery deliver the payed Items.
func (stmt *TPCStmt) HandleDelivery(client *TPCClient) {
	configs.TPrintf("Begin handle delivery from Client:" + strconv.Itoa(client.clientID))
	exist := make(map[int]bool)
	parts := make([]string, 0)
	tmp := &TPCOrder{Items: make([]*TPCOrderLine, 0)}
	BatchSize := random(1, 10)
	// TPCC standard: pay for 1~10 random orders
	for i := 0; i < BatchSize; i++ {
		v := client.payed.Pop()
		if v == nil {
			break
		}
		ware := v.(int) % NWareHouse
		order := v.(int) / NWareHouse
		if !exist[ware] {
			exist[ware] = true
			parts = append(parts, configs.OuAddress[ware])
		}
		tmp.Items = append(tmp.Items, &TPCOrderLine{
			Warehouse: ware,
			Item:      order,
		})
	}
	stmt.delivery(tmp, parts)
	configs.TPrintf("Ends handle delivery from Client:" + strconv.Itoa(client.clientID))
}

//func (c *TPCStmt) orderStatus(order *TPCOrder, parts []string) bool {
//	osTID := utils.GetTxnID()
//	osRead := coordinator.NewTX(osTID, parts, c.coordinator.TwoPhaseLockNoWaitManager)
//	for _, v := range order.Items {
//		key := utils.TransTableItem(v.Warehouse, v.Item)
//		osRead.AddRead("ORDER", configs.OuAddress[v.Warehouse], key)
//	}
//	_, ok := c.coordinator.TwoPhaseLockNoWaitManager.PreRead(osRead)
//	osWrite := coordinator.NewTX(osTID, parts, c.coordinator.TwoPhaseLockNoWaitManager)
//	ok = ok && c.coordinator.TwoPhaseLockNoWaitManager.SubmitTxn(osWrite, c.protocol, nil)
//	return ok
//}
//
//// HandleOrderStatus check the status for one random orders.
//func (c *TPCStmt) HandleOrderStatus(client *startTPCClient) {
//	configs.TPrintf("Handle order begins from Client:" + strconv.Itoa(client.clientID))
//	exist := make(map[int]bool)
//	parts := make([]string, 0)
//	tmp := &TPCOrder{Items: make([]*TPCOrderLine, 0)}
//	// Get a random order from a random warehouse.
//	v := client.allOrderIDs.Pop()
//	ware := v.(int) % NWareHouse
//	order := v.(int) / NWareHouse
//	if !exist[ware] {
//		parts = append(parts, configs.OuAddress[ware])
//		exist[ware] = true
//	}
//	tmp.Items = append(tmp.Items, &TPCOrderLine{
//		Warehouse: ware,
//		Item:  order,
//	})
//	c.orderStatus(tmp, parts)
//	configs.TPrintf("Handle order ends from Client:" + strconv.Itoa(client.clientID))
//}

type TPCStmt struct {
	coordinator  *coordinator.Context
	participants []*participant.Context
	protocol     string
	stat         *utils.Stat
	stop         int32
}

func (stmt *TPCStmt) Stop() {
	configs.LPrintf("Client stop !!!!!")
	stmt.coordinator.Close()
	atomic.StoreInt32(&stmt.stop, 1)
	if stmt.participants == nil {
		return
	}
	for _, v := range stmt.participants {
		v.Close()
	}
}

func (stmt *TPCStmt) Stopped() bool {
	return atomic.LoadInt32(&stmt.stop) != 0
}

func (stmt *TPCStmt) startTPCClient(id int) {
	client := NewTPCClient(id)
	for !stmt.Stopped() {
		for count := 0; count < 20 && !stmt.Stopped(); count++ {
			tmp := stmt.GetOrder()
			if !stmt.Stopped() {
				stmt.HandleOrder(client, tmp, stmt.stat)
			}
			if count%20 == 10 && !stmt.Stopped() {
				//c.HandleOrderStatus(client)
			}
		}
		if !stmt.Stopped() {
			stmt.HandleDelivery(client)
			//c.HandleStockLevel(client)
		}
	}
}

func (stmt *TPCStmt) RunTPC() {
	stmt.stat = utils.NewStat()
	rand.Seed(1234)
	for i := 0; i < configs.ClientRoutineNumber; i++ {
		go stmt.startTPCClient(i)
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
		stmt.stat.Clear()
		time.Sleep(configs.RunTestInterval * time.Second)
		stmt.stat.Log()
		stmt.stat.Clear()
	}
}

func (stmt *TPCStmt) TPCCTest() {
	rand.Seed(1234)
	stmt.Init()
	stmt.RunTPC()
}
