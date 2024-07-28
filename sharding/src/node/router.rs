use postgres::{Client, NoTls};
extern crate users;
use super::super::utils::node_config::*;
use super::node::*;
use rust_decimal::Decimal;
use serde_yaml;
use std::collections::HashMap;
use std::sync::Arc;
use std::{
    fs,
    io::Read,
    net::{SocketAddr, TcpListener, TcpStream},
    sync::Mutex,
};
use users::get_current_username;
// use super::super::utils::sysinfo::print_available_memory;

pub struct Channel {
    stream: TcpStream,
}

/// This struct represents the Router node in the distributed system. It has the responsibility of routing the queries to the appropriate shard or shards.
#[repr(C)]
pub struct Router {
    ///  HashMap:
    ///     key: shardId
    ///     value: Shard's Client
    shards: Mutex<HashMap<String, Client>>,
    ///  HashMap:
    ///     key: Hash
    ///     value: shardId
    hash_id: Mutex<HashMap<String, String>>,
    comm_channels: Mutex<Vec<Channel>>,
    ip: Arc<str>,
    port: Arc<str>,
}

impl Router {
    /// Creates a new Router node with the given port
    pub fn new(ip: &str, port: &str) -> Self {
        // read from 'config.yaml' to get the ports
        let config_content = fs::read_to_string("../../../sharding/src/node/config.yaml")
            .expect("Should have been able to read the file");

        let config: NodeConfig =
            serde_yaml::from_str(&config_content).expect("Should have been able to parse the YAML");

        let mut shards: HashMap<String, Client> = HashMap::new();
        let mut hash_id: HashMap<String, String> = HashMap::new();

        for node in config.nodes {
            let node_ip = node.ip;
            let node_port = node.port;

            if (node_ip == ip) && (node_port == port) {
                continue;
            }

            // get username dynamically
            let username = match get_current_username() {
                Some(username) => username.to_string_lossy().to_string(),
                None => panic!("Failed to get current username"),
            };

            match Router::connect(&node_ip, &node_port, username) {
                Ok(shard_client) => {
                    println!("Connected to ip {} and port: {}", node_ip, node_port);
                    let hash = "TODO-SHARD change this".to_string();
                    shards.insert(node_port.to_string(), shard_client);
                    hash_id.insert(hash.clone(), node_port.clone().to_string());
                }
                Err(e) => {
                    println!("Failed to connect to port: {}", node_port);
                } // Do something here
            }
        }

        if shards.is_empty() {
            eprint!("Failed to connect to any of the nodes");
        };

        println!("SHARDS: {}", shards.len());
        let router = Router {
            shards: Mutex::new(shards),
            hash_id: Mutex::new(hash_id),
            comm_channels: Mutex::new(Vec::new()),
            ip: Arc::from(ip),
            port: Arc::from(port),
        };

        // tokio::spawn(async move {
        //     Router::cluster_management_protocol(&router);

        // });
        router
    }

    fn connect(ip: &str, port: &str, username: String) -> Result<Client, postgres::Error> {
        match Client::connect(
            format!(
                "host={} port={} user={} dbname=template1",
                ip, port, username
            )
            .as_str(),
            NoTls,
        ) {
            Ok(shard_client) => Ok(shard_client),
            Err(e) => Err(e),
        }
    }

    /// This function is the cluster management protocol for the Router node. It listens to incoming connections from Shards and handles them. This might be used in the future for sending routing tables, reassigning shards, rebalancing, etc.
    fn cluster_management_protocol(router: &Router) {
        let node_addr = "localhost:".to_string() + router.port.as_ref();
        let listener = TcpListener::bind(&node_addr).unwrap();
        println!("Router is listening for connections {}", node_addr);

        loop {
            let (incoming_socket, addr) = listener.accept().unwrap();
            // Store every connection in leader
            router.add_channel(incoming_socket.try_clone().expect("Error cloning socket"));

            // A task per connection
            // tokio::spawn( async move {
            //     router.handle_connection(incoming_socket, addr);
            // });
        }
    }

    fn add_channel(&self, channel_socket: TcpStream) {
        let channel = Channel {
            stream: channel_socket,
        };
        self.comm_channels.lock().unwrap().push(channel);
    }

    fn handle_connection(&self, mut channel: TcpStream, addr: SocketAddr) {
        println!("Handling connection from {}", addr);
        println!("Connections: {}", self.comm_channels.lock().unwrap().len());
        let mut buf = [0; 1024];
        match channel.read(&mut buf) {
            Ok(n) if n == 0 => {
                // Connection was closed
                println!("Connection from {} disconnected", addr);
            }
            Ok(n) => {
                if let Ok(data) = String::from_utf8(buf[..n].to_vec()) {
                    println!("Received data from {}: {}", addr, data);
                } else {
                    println!("Received invalid UTF-8 data from {}", addr);
                }
            }
            Err(e) => {
                println!("Error reading from {}: {:?}", addr, e);
            }
        }
    }

    /// Function that receives a query and checks for shards
    /// with corresponding data
    pub fn get_shards_for_query(&self, query: &str) -> Vec<String> {
        // If it's an INSERT query return specific Shards
        if query.to_uppercase().starts_with("INSERT") {
            println!("Query is INSERT");

            // TODO-SHARD: Elegir un shard, el que tenga menor cargo o algo, etc
            return vec!["5433".to_string()];
        } else {
            // Return all shards
            println!("Returning all shards");
            let shards = self.hash_id.lock().unwrap();
            shards.values().cloned().collect()
        }
    }
}

impl NodeRole for Router {
    fn send_query(&mut self, query: &str) -> bool {
        println!("Router send_query called with query: {:?}", query);

        let shards = self.get_shards_for_query(query);

        if shards.len() == 0 {
            eprintln!("No shards found for the query");
            return false;
        }

        for shard in shards {
            if let Some(mut shard) = self.shards.lock().unwrap().get_mut(&shard) {
                let rows = match shard.query(query, &[]) {
                    Ok(rows) => rows,
                    Err(e) => {
                        eprintln!("Failed to send the query to the shard: {:?}", e);
                        return false;
                    }
                };

                for row in rows {
                    let id: i32 = row.get(0);
                    let name: &str = row.get(1);
                    let position: &str = row.get(2);
                    let salary: Decimal = row.get(3);
                    println!(
                        "QUERY RESULT: id: {}, name: {}, position: {}, salary: {}",
                        id, name, position, salary
                    );
                }
            } else {
                eprintln!("Shard not found");
                return false;
            }
        }
        true
    }
}
