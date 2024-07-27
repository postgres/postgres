package coordinator

import (
	"FC/configs"
	"FC/network/participant"
	"context"
)

func TestKit() (*Context, []*participant.Context) {
	stmt := &Context{}
	ctx := context.WithValue(context.Background(), "store_list",
		[]string{
			configs.BenchmarkStorage,
			configs.PostgreSQL,
			configs.PostgreSQL})
	paStmt := participant.TestKit(ctx)
	var Arg = []string{"*", "*", "127.0.0.1:5001", "5"}
	ch := make(chan bool)
	go begin(stmt, Arg, ch)
	<-ch
	return stmt, paStmt
}

func TPCCTestKit(ctx context.Context) (*Context, []*participant.Context) {
	stmt := &Context{}
	paStmt := participant.TPCCParticipantKit(ctx)
	var Arg = []string{"*", "*", "127.0.0.1:5001"}
	ch := make(chan bool)
	go begin(stmt, Arg, ch)
	<-ch
	return stmt, paStmt
}

func YCSBTestKit(ctx context.Context) (*Context, []*participant.Context) {
	stmt := &Context{}
	paStmt := participant.YCSBParticipantKit(ctx)
	var Arg = []string{"*", "*", "127.0.0.1:5001"}
	ch := make(chan bool)
	go begin(stmt, Arg, ch)
	<-ch
	return stmt, paStmt
}

func NormalKit(id string) *Context {
	stmt := &Context{}
	var Arg = []string{"*", "*", id}
	ch := make(chan bool)
	go begin(stmt, Arg, ch)
	<-ch
	return stmt
}
