package benchmark

import (
	"FC/configs"
)

func TestYCSB(protocol string, addr string) {
	st := YCSBStmt{}
	configs.CoordinatorServerAddress = addr
	configs.SetProtocol(protocol)
	st.YCSBTest()
	st.Stop()
}

func TestTPC(protocol string, addr string) {
	st := TPCStmt{}
	configs.CoordinatorServerAddress = addr
	configs.SetProtocol(protocol)
	st.TPCCTest()
	st.Stop()
}
