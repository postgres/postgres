package storage

import (
	"FC/configs"
	"context"
	"math/rand"
	"time"
)

// NewKV external API for creating a Local KV.
func NewKV(shardID string, len int, store string, delay time.Duration) *Shard {
	return newShardKV(shardID, store, delay)
}

func (c *Shard) GenTestValue() *RowData {
	return WrapTestValue(rand.Intn(10000))
}

func WrapTestValue(val int) *RowData {
	value := NewRowDataWithLength(1)
	value.SetAttribute(0, val)
	return value
}

func WrapYCSBTestValue(val string) *RowData {
	value := NewRowDataWithLength(10)
	for i := 0; i < 10; i++ {
		value.SetAttribute(uint(i), val)
	}
	return value
}

func WrapTPCCTestValue(table string, val interface{}) *RowData {
	attrNum := 0
	switch table {
	case configs.DISTRICT:
		attrNum = 10
	case configs.WAREHOUSE, configs.CUSTOMER, configs.STOCK, configs.HISTORY:
		attrNum = 8
	case configs.ITEM:
		attrNum = 5
	default:
		panic("invalid table name appended here.")
	}
	value := NewRowDataWithLength(attrNum)
	for i := 0; i < attrNum; i++ {
		value.SetAttribute(uint(i), val)
	}
	return value
}

func Testkit(shardID string, store string) *Shard {
	ta := newShardKV(shardID, store, 0)
	mainTB := ta.AddTable("MAIN", 1)
	for i := 0; i < configs.NumberOfRecordsPerShard; i++ {
		value := NewRowData(mainTB)
		value.SetAttribute(0, i+3)
		ta.AddRow("MAIN", uint64(i), value)
	}
	return ta
}

func YCSBStorageKit(ctx context.Context, shardID string) *Shard {
	ta := newShardKV(shardID, ctx.Value("store").(string), ctx.Value("delay").(time.Duration))
	ycsbMainTB := ta.AddTable("YCSB_MAIN", 10)
	for i := 0; i < configs.NumberOfRecordsPerShard; i++ {
		value := NewRowData(ycsbMainTB)
		for f := configs.F0; f <= configs.F9; f++ {
			value.SetAttribute(uint(f), "init_value")
		}
		ta.AddRow("YCSB_MAIN", uint64(i), value)
	}
	return ta
}

const charset = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"

func genRandString(length int) string {
	Ln := len(charset)
	result := make([]byte, length)
	for i := range result {
		result[i] = charset[rand.Intn(Ln)]
	}

	return string(result)
}

func (c *Shard) AddRow(tb string, key uint64, value *RowData) {
	for !c.Insert(tb, key, value) {
	}
}

func GetTableKey(tab string, wid int, did int, cid int, oid int, sid int) int {
	switch tab {
	case configs.WAREHOUSE:
		return wid
	case configs.DISTRICT:
		return wid*10 + did
	case configs.CUSTOMER:
		return GetTableKey(configs.DISTRICT, wid, did, cid, oid, sid)*3000 + cid
	//case configs.NEWORDER:
	//	return GetTableKey(configs.DISTRICT, wid, did, cid, oid)
	case configs.STOCK:
		return wid*10000 + sid
	case configs.ORDER:
		return wid*1000 + oid
	default:
		return 0
	}
}

func (c *Shard) initWarehouseTable(whID int) {
	wh, ok := c.tables.Load(configs.WAREHOUSE)
	configs.Assert(ok, "table misses")
	value := NewRowData(wh.(*Table))
	primaryKey := GetTableKey(configs.WAREHOUSE, whID, 0, 0, 0, 0)
	value.SetAttribute(configs.WhId, primaryKey)
	value.SetAttribute(configs.WhName, genRandString(6))
	value.SetAttribute(configs.WhStreet, genRandString(20))
	value.SetAttribute(configs.WhCity, genRandString(10))
	value.SetAttribute(configs.WhState, genRandString(2))
	value.SetAttribute(configs.WhZip, genRandString(9))
	value.SetAttribute(configs.WhTax, rand.Float32()*0.2)
	value.SetAttribute(configs.WhYTD, 300000.00)
	c.AddRow(configs.WAREHOUSE, uint64(primaryKey), value)
}

func (c *Shard) initDistrictTable(whID int) {
	ds, ok := c.tables.Load(configs.DISTRICT)
	configs.Assert(ok, "table misses")
	for did := 0; did < 10; did++ {
		primaryKey := GetTableKey(configs.DISTRICT, whID, did, 0, 0, 0)
		value := NewRowData(ds.(*Table))
		value.SetAttribute(configs.DId, primaryKey)
		value.SetAttribute(configs.DWhId, whID)
		value.SetAttribute(configs.DName, genRandString(6))
		value.SetAttribute(configs.DStreet, genRandString(20))
		value.SetAttribute(configs.DCity, genRandString(10))
		value.SetAttribute(configs.DState, genRandString(2))
		value.SetAttribute(configs.DZip, genRandString(9))
		value.SetAttribute(configs.DTax, rand.Float32()*0.2)
		value.SetAttribute(configs.DYTD, 300000.00)
		value.SetAttribute(configs.DNextOrder, 3001)
		c.AddRow(configs.DISTRICT, uint64(primaryKey), value)
	}
}

func (c *Shard) initStockTable(whID int) {
	ds, ok := c.tables.Load(configs.STOCK)
	configs.Assert(ok, "table misses")
	for sid := 0; sid < 10000; sid++ {
		primaryKey := GetTableKey(configs.STOCK, whID, 0, 0, 0, sid)
		value := NewRowData(ds.(*Table))
		value.SetAttribute(configs.SIId, sid)
		value.SetAttribute(configs.SWId, whID)
		value.SetAttribute(configs.SQuantity, rand.Intn(900)+100)
		value.SetAttribute(configs.SRemoteCnt, 0)
		value.SetAttribute(configs.SRowData, genRandString(100))
		value.SetAttribute(configs.SYTD, 0)
		value.SetAttribute(configs.SOrderCnt, 0)
		value.SetAttribute(configs.SData, genRandString(30))
		c.AddRow(configs.STOCK, uint64(primaryKey), value)
	}
}

func (c *Shard) initCustomerTable(whID int, dID int) {
	ds, ok := c.tables.Load(configs.CUSTOMER)
	configs.Assert(ok, "table misses")
	for cid := 0; cid < 3000; cid++ {
		primaryKey := GetTableKey(configs.CUSTOMER, whID, dID, cid, 0, 0)
		value := NewRowData(ds.(*Table))
		value.SetAttribute(configs.CId, cid)
		value.SetAttribute(configs.CDid, dID)
		value.SetAttribute(configs.CWid, whID)
		value.SetAttribute(configs.CName, genRandString(20))
		value.SetAttribute(configs.CStreet, genRandString(30))
		value.SetAttribute(configs.CCity, genRandString(10))
		value.SetAttribute(configs.CState, genRandString(2))
		value.SetAttribute(configs.CZip, genRandString(9))
		value.SetAttribute(configs.CPhone, genRandString(16))
		value.SetAttribute(configs.CSince, 0)
		value.SetAttribute(configs.CCreditLim, 50000)
		value.SetAttribute(configs.CDeliveryCnt, 0)
		value.SetAttribute(configs.CData, genRandString(400))
		value.SetAttribute(configs.CCredit, "DC")
		value.SetAttribute(configs.CDiscount, rand.Float32()/2)
		value.SetAttribute(configs.CBalance, -10.0)
		value.SetAttribute(configs.CYTDPayment, 10.0)
		value.SetAttribute(configs.CPaymentCnt, 1)
		c.AddRow(configs.CUSTOMER, uint64(primaryKey), value)
	}
}

func (c *Shard) initHistoryTable(whID int, dID int, cID int) {
	ds, ok := c.tables.Load(configs.HISTORY)
	configs.Assert(ok, "table misses")
	primaryKey := ds.(*Table).GenPrimaryKey()
	value := NewRowData(ds.(*Table))
	value.SetAttribute(configs.HCId, cID)
	value.SetAttribute(configs.HCDId, dID)
	value.SetAttribute(configs.HDid, dID)
	value.SetAttribute(configs.HCWId, whID)
	value.SetAttribute(configs.HWid, whID)
	value.SetAttribute(configs.HDate, 0)
	value.SetAttribute(configs.HAmount, 10.0)
	value.SetAttribute(configs.HData, genRandString(20))
	c.AddRow(configs.HISTORY, uint64(primaryKey), value)
}

func (c *Shard) initItemTable() {
	ds, ok := c.tables.Load(configs.ITEM)
	configs.Assert(ok, "table misses")
	for i := 0; i < 10000; i++ {
		primaryKey := i
		value := NewRowData(ds.(*Table))
		value.SetAttribute(configs.IId, primaryKey)
		value.SetAttribute(configs.IImId, rand.Intn(10000))
		value.SetAttribute(configs.IName, genRandString(20))
		value.SetAttribute(configs.IPrice, rand.Float32()*100)
		value.SetAttribute(configs.IData, "original")
		c.AddRow(configs.ITEM, uint64(primaryKey), value)
	}
}

func (c *Shard) initOrderTable(whID int) {
	ds, ok := c.tables.Load(configs.ORDER)
	configs.Assert(ok, "table misses")
	for i := 0; i < 1000; i++ {
		primaryKey := GetTableKey(configs.ORDER, whID, 0, 0, i, 0)
		value := NewRowData(ds.(*Table))
		value.SetAttribute(configs.OId, primaryKey)
		value.SetAttribute(configs.IImId, rand.Intn(10000))
		value.SetAttribute(configs.IName, genRandString(20))
		value.SetAttribute(configs.IPrice, rand.Float32()*100)
		value.SetAttribute(configs.IData, "original")
		c.AddRow(configs.ORDER, uint64(primaryKey), value)
	}
}

func (c *Shard) initWarehouse(whID int) {
	if whID == 0 {
		c.initItemTable()
	}
	c.initWarehouseTable(whID)
	c.initDistrictTable(whID)
	c.initStockTable(whID)
	c.initOrderTable(whID)
	//for did := 0; did < 10; did++ {
	//	c.initCustomerTable(whID, did)
	//	for cid := 0; cid < 3000; cid++ {
	//		c.initHistoryTable(whID, did, cid)
	//	}
	//}
}

// TPCCStorageKit initialize the warehouse here.
func TPCCStorageKit(ctx context.Context, shardID string) *Shard {
	rand.Seed(time.Now().UnixNano())
	ta := newShardKV(shardID, ctx.Value("store").(string), 0)
	ta.AddTable(configs.WAREHOUSE, 8)
	ta.AddTable(configs.DISTRICT, 10)
	ta.AddTable(configs.STOCK, 8)
	ta.AddTable(configs.ITEM, 5)
	//ta.AddTable(configs.CUSTOMER, 18)
	//ta.AddTable(configs.HISTORY, 8)
	ta.AddTable(configs.ORDER, 5)
	for i := 0; i < configs.NumberOfWareHousePerShards; i++ {
		ta.initWarehouse(i)
	}
	return ta
}

//var stockLock = sync.Mutex{}
//
//func TransTableItem(tableName string, ware int, key int) uint64 {
//	if tableName == "order" {
//		return uint64(key)
//	} else {
//		return uint64(100000 + 10000*ware + key)
//	}
//}
//
//func ToRow(value interface{}) *RowData {
//	res := NewRowDataWithLength(1)
//	res.Value[0] = value
//	return res
//}
//
//// LoadStock load the history data of each warehouse for TPC-C benchmark
//func (c *Shard) LoadStock() {
//	stockLock.Lock()
//	defer stockLock.Unlock()
//	file, err := os.Open("./benchmark/data/stock.csv")
//	if err != nil {
//		file, err = os.Open("../benchmark/data/stock.csv")
//	}
//	configs.CheckError(err)
//	defer file.Close()
//	s := csv.NewReader(file)
//	cnt := 0
//	for {
//		row, err := s.Read()
//		if err == io.EOF {
//			break
//		}
//		item, err := strconv.Atoi(row[0])
//		configs.CheckError(err)
//		ware, err := strconv.Atoi(row[1])
//		configs.CheckError(err)
//		count, err := strconv.Atoi(row[2])
//		configs.CheckError(err)
//		ware--
//		if configs.OuAddress[ware] == c.shardID {
//			cnt++
//			c.Update("MAIN", uint64(TransTableItem("stock", ware, item)), ToRow(count))
//		}
//	}
//	configs.TPrintf("Initialize the stock over")
//}
