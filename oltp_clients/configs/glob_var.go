package configs

import (
	"github.com/jackc/pgx/v4"
	"time"
)

// Debugging parameters.
var (
	ShowDebugInfo              = false
	ShowWarnings               = ShowDebugInfo
	ShowTestInfo               = ShowDebugInfo
	ShowRobustnessLevelChanges = ShowDebugInfo
	SpeedTestBatchPerThread    = 1000
	LogToFile                  = true
	ProfileStore               = false
	TraceFile                  = false
)

// Status codes.
const (
	// PreRead et,al. Codes for acp messages
	PreRead          string = "[msg, status] pre-read message/status"
	PreWrite         string = "[msg, status] ore-write message/status"
	FCProposed       string = "[msg, status] FC propose phase"
	Commit           string = "[msg, status] transaction is committed"
	Abort            string = "[msg, status] transaction is aborted"
	PreCommit        string = "[msg, status] 3PC agreement on commit"
	Finished         string = "transaction finished"
	ReadUnsuccessful string = "[msg] ACK message for unsuccessful pre-read"
	ReadSuccess      string = "[msg] ACK message for successful pre-read"
	PreWriteACK      string = "[msg] pre-write response message"
	FCResults        string = "[msg] the fc result message for the propose phase (vote, decision)"
	FCGossipVotes    string = "[msg] the gossip message carrying FCff votes"
	PreCommitACK     string = "[msg] ACK message fore pre commit message"

	// ThreePC et,al. the protocol codes.
	ThreePC    = "3PC"
	TwoPC      = "2PC"
	LearnedC   = "LearnedCommit"
	FC         = "FC"
	FCff       = "FCff"
	FCcf       = "FCcf"
	PAC        = "C-PAC"
	EasyCommit = "EasyCommit"
	GPAC       = "G-PAC"
	NoACP      = "No ACP"

	// LockNone et,al. the lock status codes.
	LockNone      = 0
	LockShared    = 1
	LockExclusive = 2
	LockWait      = 0
	LockAbort     = 1
	LockSucceed   = 2

	Normal      = "normal"
	Exponential = "exp"
	Plain       = "plain"

	BenchmarkStorage = "benchmark"
	MongoDB          = "mongo"
	PostgreSQL       = "sql"

	MongoDBLink = "mongodb://tester:123@localhost:27019/flexi"

	// Serializable ... The isolation levels.
	Serializable = "Serializable"

	// TwoPhaseLockNoWait ... The concurrency control algorithms.
	TwoPhaseLockNoWait  = "2PL_NW"
	TwoPhaseLockWaitDie = "2PL_WD"
	OptimisticCC        = "OCC"
	VeryLightLock       = "VLL"
)

// System parameters.
const (
	MaxConnectionHandler           = 16
	MaxAccessesPerTxn              = 64
	CrashFailureTimeout            = 5 * time.Second
	BTreeOrder                     = 16
	DeferredInsert                 = false
	LogBatchInterval               = 10 * time.Millisecond
	TPCRecordPerShard              = 200000
	WarmUpTime                     = 5 * time.Second
	ParticipantWarmUpTime          = 4 * time.Second
	RunTestInterval                = 5
	RunParticipantProfilerInterval = 10
	DelayStaticPreHeat             = 2 * time.Second
	CrashPeriod                    = time.Second
	DelayPeriod                    = time.Second
	NoBlindWrite                   = false
	MaxRetry                       = 5
	InitPenalty4Abort              = 1 * time.Millisecond
	SSISwitchThr                   = 0.85
)

// Workload parameters that could be changed by args.
var (
	Benchmark                  = "ycsb"
	EnableReplication          = false
	EnableQuickPreWriteAbort   = false
	UseWAL                     = false
	NumberOfRecordsPerShard    = 10000
	NumberOfReplicas           = 3
	NumberOfShards             = 3
	NumberOfWareHousePerShards = 64
	TransactionLength          = 16
	ReadPercentage             = 0.5
	YCSBDataSkewness           = 0.9
	CrossShardTXNPercentage    = 100
	ShardsPerTransaction       = 2
	ClientRoutineNumber        = 10
	SelectedACP                = "FCff"
	SelectedCC                 = TwoPhaseLockNoWait
	ConfigFileLocation         = "./configs/remote.json"
	CoordinatorServerAddress   = "127.0.0.1:5001"
	SimulateClientSideDelay    = false
	StoredProcedure            = true
	NetWorkDelayParameter      = float64(1.5)
	Distribution               = Normal
	DelayStdDev                = 100 * time.Millisecond
	ExpBaseDelay               = 2 * time.Millisecond
	TimeElapsedTest            = false
	InjectDelay                = 0 * time.Millisecond
	DefaultIsolationLevel      = pgx.Serializable
	LockType                   = "none"
	//StorageType                = BenchmarkStorage // benchmark
)

const (
	LearnedCC     = "learned locking strategy"
	NoWait2PL     = "2pl no wait"
	WaitDie2PL    = "2pl wait die"
	Native2PL     = "2pl implemented into postgresql"
	OCC           = "occ"
	NoLock        = "no lock"
	RuleBasedLock = "rule-based locking strategy"

	// WAREHOUSE ORDER, ... TPC-C Table names.
	WAREHOUSE = "TPCC_Warehouse"
	STOCK     = "TPCC_Stock"
	ORDER     = "TPCC_Order"
	DISTRICT  = "TPCC_District"
	CUSTOMER  = "TPCC_Customer"
	ITEM      = "TPCC_Item"
	HISTORY   = "TPCC_History"
	// use CSV files.
	//ORDER     = "TPCC_Order"
	//ORDERLINE = "TPCC_OrderLine"
	//NEWORDER  = "TPCC_NewOrder"

	// F0 F1, ... YCSB attributes
	F0 = 0
	F9 = 9
	//F1 = 1
	//F2 = 2
	//F3 = 3
	//F4 = 4
	//F5 = 5
	//F6 = 6
	//F8 = 8

	// WhId ... the fields of TPC-C warehouse table
	WhId     = 0
	WhName   = 1
	WhStreet = 2
	WhCity   = 3
	WhState  = 4
	WhZip    = 5
	WhTax    = 6
	WhYTD    = 7

	// DId ... the fields of TPC-C district table
	DId        = 0
	DWhId      = 1
	DName      = 2
	DStreet    = 3
	DCity      = 4
	DState     = 5
	DZip       = 6
	DTax       = 7
	DYTD       = 8
	DNextOrder = 9

	// IId ... the fields of TPC-C item table
	IId    = 0
	IImId  = 1
	IName  = 2
	IPrice = 3
	IData  = 4

	// SIId ... the fields of TPC-C stock table
	SIId       = 0
	SWId       = 1
	SQuantity  = 2
	SRemoteCnt = 3
	SRowData   = 4
	SYTD       = 5
	SOrderCnt  = 6
	SData      = 7

	// CId ... the fields of TPC-C customer table
	CId          = 0
	CDid         = 1
	CWid         = 2
	CName        = 3
	CStreet      = 4
	CCity        = 5
	CState       = 6
	CZip         = 7
	CPhone       = 8
	CSince       = 9
	CCreditLim   = 10
	CDeliveryCnt = 11
	CData        = 12
	CCredit      = 13
	CDiscount    = 14
	CBalance     = 15
	CYTDPayment  = 16
	CPaymentCnt  = 17

	// HCId ... the fields of TPC-C history table
	HCId    = 0
	HCDId   = 1
	HDid    = 2
	HCWId   = 3
	HWid    = 4
	HDate   = 5
	HAmount = 6
	HData   = 7

	// OId ... the fields of TPC-C order table
	OId     = 0
	OWId    = 1
	OCId    = 2
	ODId    = 3
	OAmount = 4
)

var (
	ServerAutoCrashEnabled = false
	ExpectedCrashTime      = 20 * time.Second
	NetworkDisruptEnabled  = false
	ExpectedDelayTime      = 20 * time.Second
)

const (
	DetectorDownBatchSize = 5
)

// FCff parameters
var (
	FCMinRobustnessLevel = 2
	DetectorInitWaitCnt  = 0
)

func SetDown(d int) {
	DetectorInitWaitCnt = d
}

func SetFailureInjection(crashPeriod int, delayPeriod int) {
	if delayPeriod >= 0 {
		NetworkDisruptEnabled = true
		ExpectedDelayTime = time.Duration(delayPeriod) * time.Millisecond
	} else {
		NetworkDisruptEnabled = false
		ExpectedDelayTime = 0
	}
	if crashPeriod >= 0 {
		ServerAutoCrashEnabled = true
		ExpectedCrashTime = time.Duration(crashPeriod) * time.Millisecond
	} else {
		ServerAutoCrashEnabled = false
		ExpectedCrashTime = 0
	}
}

func SetMinLevel(l int) {
	FCMinRobustnessLevel = l
}

// DO NOT MODIFY HERE, global variables used by program for test //
var (
	TestCF int32 = 0
	TestNF int32 = 0
)

func SetProtocol(pro string) {
	if pro == "2pc" {
		SelectedACP = TwoPC
	} else if pro == "3pc" {
		SelectedACP = ThreePC
	} else if pro == "fc" {
		SelectedACP = FC
	} else if pro == "fcff" {
		SelectedACP = FCff
	} else if pro == "fccf" {
		SelectedACP = FCcf
	} else if pro == "pac" {
		SelectedACP = PAC
	} else if pro == "easy" {
		SelectedACP = EasyCommit
	} else if pro == "gpac" {
		SelectedACP = GPAC
	} else if pro == "learned" {
		SelectedACP = LearnedC
	} else {
		panic("incorrect protocol flag: shall be 2pc, 3pc, pac, fc, easy, or gpac")
	}
}
