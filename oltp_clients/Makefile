.PHONY: build run-server tpc-local-test tpc ycsb local tmp msgtest exp mi heu skew

all: test build

# docker exec -it fc-node make build
build:
	@go build -o ./bin/fc-server ./fc-server/main.go

# docker run -dt --name=pg-wrap --network=host pg-cc sh
# docker exec -it fc-node tc qdisc add dev ens1f1 root handle 1: prio bands 4
# docker exec -it fc-node tc qdisc add dev ens1f1 parent 1:4 handle 40: netem delay 10ms
# docker exec -it fc-node tc filter add dev ens1f1 protocol ip parent 1:0 prio 4 u32 match ip dport 2001 0xffff flowid 1:4
# docker exec -it fc-node tc filter del dev ens1f1 pref 4
# docker exec -it fc-node tc qdisc del dev ens1f1 root


# docker exec -it fc-node tc qdisc add dev eth0 root netem delay 10ms

# docker run --name pg -e POSTGRES_PASSWORD=flexicommit -p 5432:5432 -d postgres
# docker run --name mongo -p 27017:27019 mongo

build-docker:
	@docker rmi -f fc
	@rm -f fc-docker.tar
	@docker build -t pg-cc .
	@docker save -o fc-docker.tar pg-cc

delay:
	@tc qdisc add dev lo root handle 1: prio bands 4
	@tc qdisc add dev lo parent 1:4 handle 40: netem delay 5ms 2ms
	@tc filter add dev lo protocol ip parent 1:0 prio 4 u32  match ip dport 6001 0xffff flowid 1:4
	@tc filter add dev lo protocol ip parent 1:0 prio 4 u32  match ip dport 6002 0xffff flowid 1:4
	@tc filter add dev lo protocol ip parent 1:0 prio 4 u32  match ip dport 6003 0xffff flowid 1:4
	@tc filter add dev lo protocol ip parent 1:0 prio 4 u32  match ip dport 6004 0xffff flowid 1:4
	@tc filter add dev lo protocol ip parent 1:0 prio 4 u32  match ip dport 6005 0xffff flowid 1:4
	@tc filter add dev lo protocol ip parent 1:0 prio 4 u32  match ip dport 6006 0xffff flowid 1:4
	@tc filter add dev lo protocol ip parent 1:0 prio 4 u32  match ip dport 6007 0xffff flowid 1:4
	@tc filter add dev lo protocol ip parent 1:0 prio 4 u32  match ip dport 6008 0xffff flowid 1:4
	@tc filter add dev lo protocol ip parent 1:0 prio 4 u32  match ip dport 5001 0xffff flowid 1:4

rm_delay:
	@tc filter del dev lo pref 4
	@tc qdisc  del dev lo root

test:
	bash ./scripts/run_go_unittest.sh

#buildrpc:
#	@cd downserver
#	@python -m grpc_tools.protoc --python_out=. --grpc_python_out=. -I. rpc.proto
#	@protoc --go_out=plugins=grpc:. rpc.proto

clear:
	@python3 scripts/recover_txn.py

exp:
	@make build
	@python3 experiment.py

down:
	@python3 downserver/main.py 68

profiling:
	@make build
	@./bin/fc-server -node=c -addr=127.0.0.1:5001 -local -p=fc -cpu_prof="fc.prof" -store=mongo

check:
	@make build
#	./bin/fc-server -node=c -local=true -addr=127.0.0.1:5001 -c=16 -skew=0.5 -rw=0.8 -lock=none -iso=s
#	./bin/fc-server -node=c -local=true -addr=127.0.0.1:5001 -c=16 -skew=0.5 -rw=0.8 -lock=learned -iso=none
	./bin/fc-server -node=c -local=true -addr=127.0.0.1:5001 -c=16 -skew=0.5 -rw=0.5 -lock=learned -iso=none
#	./bin/fc-server -node=c -local=true -addr=127.0.0.1:5001 -c=16 -skew=0.8 -rw=0.5 -lock=none -iso=s
#	./bin/fc-server -node=c -local=true -addr=127.0.0.1:5001 -c=16 -skew=0 -rw=0.8 -lock=none -iso=s
#	./bin/fc-server -node=c -local=true -addr=127.0.0.1:5001 -c=16 -skew=0.0 -rw=0.8 -lock=none -iso=s
#	./bin/fc-server -node=c -local=true -addr=127.0.0.1:5001 -c=16 -skew=0.0 -rw=0.8 -lock=learned
#	./bin/fc-server -node=c -local=true -addr=127.0.0.1:5001 -c=16 -skew=0.0 -rw=0.8 -lock=none -iso=s
#	./bin/fc-server -node=c -local=true -addr=127.0.0.1:5001 -c=16 -skew=0.0 -rw=0.2
#	./bin/fc-server -node=c -local=true -addr=127.0.0.1:5001 -c=16 -skew=0.9 -rw=0.2 -iso=s -lock=none
