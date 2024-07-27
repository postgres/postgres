from xmlrpc.client import ServerProxy


if __name__ == '__main__':
    s = ServerProxy("http://localhost:5003")
    for i in range(10):
        print(s.action(1, "1.1"))