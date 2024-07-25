import yaml
import sys

def get_ports(config_file):
    with open(config_file, 'r') as file:
        config = yaml.safe_load(file)
        ports = [node['port'] for node in config['nodes']]
        for port in ports:
            print(port)

if __name__ == "__main__":
    config_file = sys.argv[1]
    get_ports(config_file)