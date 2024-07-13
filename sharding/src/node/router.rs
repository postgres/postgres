use postgres::{Client, NoTls};
extern crate users;
use rust_decimal::Decimal;
use users::get_current_username;
use std::{fs, io::Read, net::{SocketAddr, TcpListener, TcpStream}, sync::Mutex};
use super::node::*;
use std::sync::Arc;

pub struct Channel {
    stream: TcpStream,
}

/// This struct represents the Router node in the distributed system. It has the responsibility of routing the queries to the appropriate shard or shards.
#[repr(C)]
pub struct Router {
    shards: Mutex<Vec<Client>>,
    comm_channels: Mutex<Vec<Channel>>,
    port: Arc<str>,
    // TODO-SHARD: add hash table for routing
}

impl Router {
    /// Creates a new Router node with the given port
    pub fn new(port: &str) -> Self {

        // read from 'ports.txt' to get the ports
        // TODO-SHARD this path needs to be fixed
        let contents = fs::read_to_string("/Users/aldanarastrelli/Documents/Aldana/distributed-postgres/sharding/src/node/ports.txt")
        .expect("Should have been able to read the file");
        let ports: Vec<&str> = contents.split("\n").collect();
        let mut shards = Vec::new();
        for client_port in ports {
            if client_port == port {
                continue
            }
            match Router::connect(client_port) {
                Ok(client) => {
                    println!("Connected to port: {}", client_port);
                    shards.push(client)
                },
                Err(e) => {
                    println!("Failed to connect to port: {}", client_port);
                }, // Do something here
            }
        }
        if shards.is_empty() {
            eprint!("Failed to connect to any of the nodes");
        };

        let router = Router {
            shards: Mutex::new(shards),
            comm_channels: Mutex::new(Vec::new()),
            port: Arc::from(port),
        };
        
        // tokio::spawn(async move {
        //     Router::cluster_management_protocol(&router);
            
        // });
        router
    }

    fn connect(port: &str) -> Result<Client, postgres::Error> {
        // get username dynamically
        let username = match get_current_username() {
            Some(username) => username.to_string_lossy().to_string(),
            None => panic!("Failed to get current username"),
        };
        println!("Username found: {:?}", username);
        
        // TODO-SHARD host should be dynamic or from configuration file
        match Client::connect(format!("host=127.0.0.1 port={} user={} dbname=template1", port, username).as_str(), NoTls) {
            Ok(client) => Ok(client),
            Err(e) => {
                Err(e)
            }
        }
    }

    /// This function is the cluster management protocol for the Router node. It listens to incoming connections from clients and handles them. This might be used in the future for sending routing tables, reassigning shards, rebalancing, etc.
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
}

impl NodeRole for Router {
    fn send_query(&mut self, query: &str) -> bool {
        println!("Router send_query called with query: {:?}", query);
        // TODO-SHARD: implement the routing logic
        
        // here

        // send the query to the first shard for now. When routing logic is added, the change needed is:
        // get the client from the routing logic, delete the first line down here, leave the rest to execute for the found client.
        // If the query needs to be sent to multiple shards, then the logic should be changed to send the query to all the shards (found in the routing logic or all the shards in general).
        let client = &mut self.shards.lock().unwrap()[0];
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
        true
    }
}
