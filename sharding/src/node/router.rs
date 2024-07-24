use postgres::{Client, NoTls};
extern crate users;
use rust_decimal::Decimal;
use users::get_current_username;
use std::{fs, io::Read, net::{SocketAddr, TcpListener, TcpStream}, sync::Mutex};
use std::collections::HashMap;
use std::hash::Hash;
use super::node::*;
use std::sync::Arc;
use regex::Regex;
use serde_yaml;
use super::super::utils::hash::*;
use super::super::utils::server_config::*;

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
    // TODO-SHARD: add hash table for routing
}

impl Router {
    /// Creates a new Router node with the given port
    pub fn new(ip: &str, port: &str) -> Self {

        // read from 'config.yaml' to get the ports
        let config_content = fs::read_to_string("../../../sharding/src/node/config.yaml")
        .expect("Should have been able to read the file");

        let config: ServerConfig = serde_yaml::from_str(&config_content)
        .expect("Should have been able to parse the YAML");

        let mut shards : HashMap<String,Client> = HashMap::new();
        let mut hash_id : HashMap<String, String> = HashMap::new();

        for server in config.servers {
            let server_ip = server.ip;
            let server_port = server.port;

            if (server_ip == ip) && (server_port == port) {
                continue
            }

            // get username dynamically
            let username = match get_current_username() {
                Some(username) => username.to_string_lossy().to_string(),
                None => panic!("Failed to get current username"),
            };

            match Router::connect(&server_ip, &server_port, username) {
                Ok(shard_client) => {
                    println!("Connected to ip {} and port: {}", server_ip, server_port);
                    let hash = hash_shard(&server_ip, &server_port);
                    shards.insert(server_port.to_string(), shard_client);
                    hash_id.insert(hash.clone(), server_port.clone().to_string());
                },
                Err(e) => {
                    println!("Failed to connect to port: {}", server_port);
                }, // Do something here
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
        match Client::connect(format!("host={} port={} user={} dbname=template1", ip, port, username).as_str(), NoTls) {
            Ok(shard_client) => Ok(shard_client),
            Err(e) => {
                Err(e)
            }
        }
    }

    /// This function is the cluster management protocol for the Router node. It listens to incoming connections from Shards and handles them. This might be used in the future for sending routing tables, reassigning shards, rebalancing, etc.
    fn cluster_management_protocol(router: &Router) {

        let server_addr = "localhost:".to_string() + router.port.as_ref();
        let listener = TcpListener::bind(&server_addr).unwrap();
        println!("Router is listening for connections {}", server_addr);

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

    /// Function that receives a query and checks for client
    /// with corresponding data
    pub fn get_clients_for_query(&self, query: &str) -> Vec<String> {
        // If it's an INSERT query return specific Clients
        if query.to_uppercase().starts_with("INSERT") {
            //  Extract fields in Query
            let data_to_hash = Self::extract_data_from_insert_query(query);
            //  Hash every field
            let hashes = self.hash_data(data_to_hash);
            let shards = self.shards.lock().unwrap();
            let mut clients = Vec::new();
            //  For every hash, check if a client has that info
            for hash in hashes {
                if let Some(_client) = shards.get(&hash) {
                    clients.push(self.hash_id.lock().unwrap().get(&hash).unwrap().to_string());
                }
            }
            clients
        } else {
            // Return all clients
            let shards = self.hash_id.lock().unwrap();
            shards.values().cloned().collect()
        }
    }
    
    fn extract_data_from_insert_query(query: &str) -> Vec<String> {
        let re = Regex::new(r"(?i)INSERT INTO [^(]+ \([^)]*\) VALUES \(([^)]+)\)").unwrap();
        if let Some(captures) = re.captures(query) {
            if let Some(values_str) = captures.get(1) {
                return values_str.as_str()
                    .split(',')
                    .map(|s| s.trim().trim_matches('\'').to_string())
                    .collect();
            }
        }
        Vec::new()
    }
}

impl NodeRole for Router {
    fn send_query(&mut self, query: &str) -> bool {
        println!("Router send_query called with query: {:?}", query);
        //  TODO: THIS GET MUT IS DUMMY, IT MIGHT FAIL IF DATA IS IN ANOTHER CLIENT
        if let Some(mut client) = self.shards.lock().unwrap().get_mut("5434") {
            let rows = match client.query(query, &[]) {
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
            false
        }
    }
}

impl Router {


}