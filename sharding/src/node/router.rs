use postgres::{Client, NoTls};
extern crate users;
use crate::node::messages::message::{Message, MessageType};
use crate::node::shard::Shard;
use crate::utils::queries::query_is_insert;

use super::node::*;
use crate::utils::node_config::get_router_config;
use super::shard_manager::ShardManager;
use inline_colorization::*;
use rust_decimal::Decimal;
use serde_yaml;
use std::collections::{BinaryHeap, HashMap};
use std::io::Write;
use std::sync::{Arc, RwLock};
use std::{io, net::TcpStream, sync::Mutex};
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
    shard_manager: ShardManager,
    ///  HashMap:
    ///     key: Hash
    ///     value: shardId
    comm_channels: Arc<RwLock<HashMap<String, Channel>>>,
    ip: Arc<str>,
    port: Arc<str>,
}

impl Router {
    /// Creates a new Router node with the given port
    pub fn new(ip: &str, port: &str, config_path: Option<&str>) -> Self {
        let config = get_router_config(config_path);

        let mut shards: HashMap<String, Client> = HashMap::new();
        let mut comm_channels : HashMap<String, Channel> = HashMap::new();
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
                    shards.insert(node_port.to_string(), shard_client);
                    match Router::health_connect(&node_ip, &node_port) {
                        Ok(health_connection) => {
                            comm_channels.insert(node_port.to_string(), health_connection);
                            // TODO-SHARD: Send Threshold Check Message to Shard and save shard to ShardManager
                    
                            let update_message = Message::new(MessageType::Update, None);
                    
                            health_connection
                                .stream
                                .write(update_message.to_string().as_bytes())
                                .unwrap();
                        }
                        Err(e) => {
                            println!("Failed to connect to port: {}. Error: {:?}", node_port, e);
                        } // Do something here
                    }
                }
                Err(e) => {
                    println!("Failed to connect to port: {}. Error: {:?}", node_port, e);
                } // Do something here
            }
        }
        if shards.is_empty() {
            eprint!("Failed to connect to any of the nodes");
        };

        println!("SHARDS: {}", shards.len());
        let router = Router {
            shards: Mutex::new(shards),
            shard_manager: ShardManager::new(),
            comm_channels: Arc::new(RwLock::new(comm_channels)),
            ip: Arc::from(ip),
            port: Arc::from(port),
        };
        println!("conns: {}", router.comm_channels.read().unwrap().len());
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

    fn health_connect(node_ip: &str, node_port: &str) -> Result<Channel, io::Error> {
        let port = node_port.parse::<u64>().unwrap() + 1000;
        match TcpStream::connect(format!("{}:{}", node_ip, port)) {
            Ok(mut stream) => {
                println!(
                    "{color_bright_green}Health connection established with {}:{}{style_reset}",
                    node_ip, port
                );
                Ok(Channel { stream })
            }
            Err(e) => {
                println!(
                    "{color_red}Error establishing health connection with {}:{}. Error: {:?}{style_reset}",
                    node_ip, port, e
                );
                Err(e)
            }
        }
    }
    fn add_channel(&self, channel_socket: TcpStream) {
        let channel = Channel {
            stream: channel_socket,
        };
        self.comm_channels.write().unwrap().push(channel);
    }

    /// Function that receives a query and checks for shards
    /// with corresponding data
    fn get_shards_for_query(&self, query: &str) -> Vec<String> {
        // If it's an INSERT query return specific Shards
        if query_is_insert(query) {
            println!("Query is INSERT");

            // TODO-SHARD: ShardManager.pop() to get the shard with the least amount of data
            // let shard = self.shard_manager.pop().unwrap();
            return vec!["5433".to_string()];
        } else {
            // Return all shards
            println!("{color_bright_white}{style_bold}Returning all shards{style_reset}");
            self.shards.lock().unwrap().keys().cloned().collect()
        }
    }

    /// Function that iterates randomly through shards, sends a AskInsertion message and waits for a response
    fn get_welcoming_shard(&self) -> Option<String> {
        let shards = self.shards.lock().unwrap();
        // TODO-SHARD: Randomize the order of the shards or implement a better way to choose the first shard to ask
        // TODO-SHARD: Implement a timeout for the response
        // TODO-SHARD: Should this be done in parallel?
        for (shard_id, shard) in shards.iter() {
            let mut stream = self.comm_channels.get(shard_id).unwrap().stream.try_clone().unwrap();

            // Send AskInsertion message
            let message = Message::new(MessageType::AskInsertion);
            let message_string = message.to_string();
            stream.write_all(message_string.as_bytes()).unwrap();

            // Wait for response with timeout 2 seconds
            let mut response = String::new();
            stream.set_read_timeout(Some(std::time::Duration::new(2, 0))).unwrap();
            match stream.read_to_string(&mut response) {
                Ok(_) => {
                    // Parse response
                    let response_message = Message::from_string(&response).unwrap();
                    if response_message == Message::new(MessageType::Agreed) {
                        println!(
                            "{color_bright_green}Shard {} accepted the insertion{style_reset}",
                            shard_id
                        );
                        return Some(shard_id.clone());
                    } else {
                        println!(
                            "{color_red}Shard {} denied the insertion{style_reset}",
                            shard_id
                        );
                    }
                }
                Err(_) => {
                    println!(
                        "{color_red}Shard {} did not respond{style_reset}",
                        shard_id
                    );
                }
            }
        }
        None
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
            if let Some(shard) = self.shards.lock().unwrap().get_mut(&shard) {
                let rows = match shard.query(query, &[]) {
                    Ok(rows) => {
                        println!("Query executed successfully: {:?}", rows);
                        rows
                    }
                    Err(e) => {
                        eprintln!("Failed to send the query to the shard: {:?}", e);
                        return false;
                    }
                };

                // TODO-SHARD: maybe this can be encapsulated inside another trait, with `.query` included
                // TODO-SHARD: Send Update Message to Shard and re-insert into ShardManager

                // for row in rows {
                //     let id: i32 = row.get(0);
                //     let name: &str = row.get(1);
                //     let position: &str = row.get(2);
                //     let salary: Decimal = row.get(3);
                //     println!(
                //         "QUERY RESULT: id: {}, name: {}, position: {}, salary: {}",
                //         id, name, position, salary
                //     );
                // }
            } else {
                eprintln!("Shard not found");
                return false;
            }
        }
        true
    }
}
