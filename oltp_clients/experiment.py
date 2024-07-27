import json
import subprocess
import os
import time

usr_name = "hexiang"
TestBatch = 1
main_command = ""
logf = open("./logs/progress.log", "w+")

# run_client_cmd = "docker exec -it pg-wrap ./bin/fc-server -node=c -local=true"
run_client_cmd = "./bin/fc-server -node=c -local=true"
with open("./configs/local.json") as f:
    config = json.load(f)

for id_ in config["coordinators"]:
    run_client_cmd = run_client_cmd + " -addr=" + config["coordinators"][id_]


def get_client_cmd(bench, file, clients=16, cf=-1, nf=-1, skew=0.5, cross=0, length=10, txn_part=1, np=1, rw=0.5
                   , tb=10000, r=2.0, dis="exp", elapsed=False, replica=False, delay_var=0.0,
                   store="benchmark", debug="false", d=1, wh=1, iso="s", lock="none"):
    return run_client_cmd + " -bench=" + str(bench) + \
        " -c=" + str(clients) + \
        " -nf=" + str(nf) + \
        " -cf=" + str(cf) + \
        " -wh=" + str(wh) + \
        " -debug=" + str(debug) + \
        " -skew=" + str(skew) + \
        " -cross=" + str(cross) + \
        " -len=" + str(length) + \
        " -part=" + str(np) + \
        " -rw=" + str(rw) + \
        " -r=" + str(r) + \
        " -dis=" + str(dis) + \
        " -txn_part=" + str(txn_part) + \
        " -elapsed=" + str(elapsed) + \
        " -replica=" + str(replica) + \
        " -dvar=" + str(delay_var) + \
        " -d=" + str(d) + \
        " -iso=" + str(iso) + \
        " -lock=" + str(lock) + \
        " -tb=" + str(tb) + file


def call_cmd_in_ssh(address, cmd):
    cmd = "ssh " + usr_name + "@" + address + " " + cmd + ">./server.log"
    print(cmd)
    ssh = subprocess.call(cmd,
                           shell=True,
                           stdout=subprocess.PIPE,
                           stderr=subprocess.PIPE)


def execute_cmd_in_ssh(address, cmd):
    cmd = "ssh " + usr_name + "@" + address + " " + cmd + ">./server.log"
    print(cmd)
    ssh = subprocess.Popen(cmd,
                           shell=True,
                           stdout=subprocess.PIPE,
                           stderr=subprocess.PIPE)
    return ssh


def run_task(cmd):
    print(cmd)
    print(cmd, file=logf)
    logf.flush()
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE,
                         shell=True, preexec_fn=os.setsid)
    return p


def delete_extra_zero(n):
    if isinstance(n, int):
        return str(n)
    if isinstance(n, float):
        n = str(n).rstrip('0')
        if n.endswith('.'):
            n = n.rstrip('.')
        return n
    return "The type is not supported yet"


def test_connect():
    skew = 0.5
    filename = ">./results/test_connection.txt"
    p = run_task(get_client_cmd("ycsb", filename, clients=16, iso="none", lock="rule", skew=skew, rw=0.2))
    p.wait()
    filename = ">>./results/test_connection.txt"
    p = run_task(get_client_cmd("ycsb", filename, clients=16, iso="none", lock="2pl", skew=skew, rw=0.2))
    p.wait()
    filename = ">>./results/test_connection.txt"
    p = run_task(get_client_cmd("ycsb", filename, clients=16, iso="s", lock="none", skew=skew, rw=0.2))
    p.wait()


def test_contention_impact_on_algorithms():
    filename = ">./results/contention_impact_rule.txt"
    alg_list = [("none", "learned")]
    # for rw in [0.2, 0.5, 0.8]:
    #     for th in [8, 16, 32]:
    #         for skew in [0.2, 0.5, 0.8]:
    for rw in [0.5]:
        for th in [1]:
            for skew in [0.5]:
                for iso, alg in alg_list:
                    for _ in range(1):
                        p = run_task(get_client_cmd("ycsb",
                                                    filename, clients=th, iso=iso, lock=alg, rw=rw, skew=skew))
                        p.wait()
                        if filename[1] == '.':
                            filename = ">" + filename


if __name__ == '__main__':
    test_contention_impact_on_algorithms()
    logf.close()
