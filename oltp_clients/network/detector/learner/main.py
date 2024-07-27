import sys

import ql
from concurrent import futures
import time
import rpc_pb2
import grpc
import rpc_pb2_grpc

Train_Round = 500
Host = "localhost"
Port = 5003
Key_steps = [0, 1, 2, 4, 8, 16, 32, 64, 128]

class Learner:
    def __init__(self, isNF=False):
        self.mo = ql.init()
        self.eps = 0.2
        self.states = 0
        self.st = -1
        self.act = 0
        self.level = -1
        self.end = 8
        self.rem = 0
        self.lastTime = time.time()
        if not isNF:
            for i in range(7):
                self.mo.q[i][1] = 100
            self.mo.q[7][0] = 100
        else:
            self.mo.q[0][0] = 100


    def get_tps(self, re):
        interval = time.time() - self.lastTime
        if re == -1 or interval == 0:
            return -1
        kvTransaction = re / interval
        print("tps = ", kvTransaction)
        return kvTransaction

    def reset(self, level, re):
        re = self.get_tps(re)
        self.eps = max(0.0, self.eps -  0.5 / Train_Round)
        if self.eps > 0:
            if self.st != -1 and re >= 0:
                self.mo.update_transition(self.st, self.act, re, self.states,
                                             self.states == self.end)
        self.st = -1
        self.states = 0
        self.rem = 0
        self.lastTime = time.time()

    def action(self, level, re):
        re = self.get_tps(re)
        if self.rem > 0:
            self.rem -= 1
            self.lastTime = time.time()
            return 1
        self.eps = max(0.1, self.eps -  1 / Train_Round)
        if self.eps > 0 and re >= 0:
            if self.st != -1:
                self.mo.update_transition(self.st, self.act, re, self.states,
                                             self.states == self.end)
            self.act = self.mo.choose_action(self.states, self.eps)
            self.st = self.states
        else:
            self.act = self.mo.max_q(self.states)[0]
            print("train finished")

        if self.act == 0 or self.states == self.end:
            if self.eps <= 0 and self.act == 0:
                self.act = -(2**self.states + 3)
#            print("H = ", Key_steps[self.states])
            self.states = self.end
            self.reset(level, -1)
        else:
            print("H --> ", Key_steps[self.states])
            self.states += 1
            self.rem = Key_steps[self.states] - Key_steps[self.states-1]-1
        self.lastTime = time.time()
        return self.act

Learners = {
    "1.1" : Learner(False),
    "2.1" : Learner(False),
    "3.1" : Learner(False),
    "x.2" : Learner(True),
}

class Action(rpc_pb2_grpc.ActionServicer):
    def action(self, request, context):
        lev = request.level - 1
        re = request.reward
        if lev < 2:
            i = str(int(request.cid[-1]) - int('0'))
        else:
            i = "x.2"
        if lev == 0:
            Learners[i+".1"].reset(lev, re)
            Learners["x.2"].reset(lev, re)
            return rpc_pb2.Act(action = 1)
        elif lev == 1:
            i += "." + str(lev)
            kvTransaction = Learners[i].action(lev, re)
            return rpc_pb2.Act(action = kvTransaction)
        else:
            kvTransaction = Learners[i].action(lev, re)
            return rpc_pb2.Act(action = kvTransaction)

class Reset(rpc_pb2_grpc.ResetServicer):
    def reset(self, request, context):
        lev = request.level - 1
        re = request.reward
        if lev <= 1:
            i = str(int(request.cid[-1]) - int('0'))
            Learners[i+".1"].reset(lev, re)
        else:
            Learners["x.2"].reset(lev, re)
        return rpc_pb2.Act(action = 1)

if __name__ == '__main__':
    server = grpc.server(futures.ThreadPoolExecutor(max_workers=1))
    rpc_pb2_grpc.add_ResetServicer_to_server(Reset(), server)
    rpc_pb2_grpc.add_ActionServicer_to_server(Action(), server)

    timeL  =  60
    if len(sys.argv) > 1:
        timeL = int(sys.argv[1])

    server.add_insecure_port('0.0.0.0:5003')
    server.start()
    server.wait_for_termination(timeout=timeL)
    print("starting service")
    server.stop(None)