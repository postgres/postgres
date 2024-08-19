use postgres::{Client, NoTls};
extern crate users;
use crate::node::messages::message::{Message, MessageType};
use crate::node::shard::Shard;
use crate::node::shard_manager;
use crate::utils::queries::query_is_insert;

use super::node::*;
use crate::utils::node_config::get_router_config;
use super::shard_manager::ShardManager;
use inline_colorization::*;
use rust_decimal::Decimal;
use serde_yaml;
use std::collections::{BinaryHeap, HashMap};
use std::io::{Read, Write};
use std::sync::{Arc, RwLock};
use std::{
    fs, io,
    net::{SocketAddr, TcpListener, TcpStream},
    sync::Mutex,
    thread,
};
use crate::utils::node_config::get_router_config;
use crate::utils::common::get_username_dinamically;

// use super::super::utils::sysinfo::print_available_memory;

#[derive(Clone)]
pub struct Channel {
    stream: Arc<Mutex<TcpStream>>,
}

/// This struct represents the Router node in the distributed system. It has the responsibility of routing the queries to the appropriate shard or shards.
#[repr(C)]
#[derive(Clone)]
pub struct Router {
    ///  HashMap:
    ///     key: shardId
    ///     value: Shard's Client
    shards: Arc<Mutex<HashMap<String, Client>>>,
    shard_manager: Arc<ShardManager>,
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
        let mut comm_channels: HashMap<String, Channel> = HashMap::new();
        let mut shard_manager = ShardManager::new();
        for node in config.nodes {
            let node_ip = node.ip;
            let node_port = node.port;

            if (node_ip == ip) && (node_port == port) {
                continue;
            }

            // get username dynamically
            let username = get_username_dinamically();

            match Router::connect(&node_ip, &node_port, username) {
                Ok(shard_client) => {
                    println!("Connected to ip {} and port: {}", node_ip, node_port);
                    shards.insert(node_port.to_string(), shard_client);
                    match Router::get_shard_channel(&node_ip, &node_port) {
                        Ok(mut health_connection) => {
                            comm_channels.insert(node_port.to_string(), health_connection.clone());
                            
                            // Send InitConnection Message to Shard and save shard to ShardManager
                            let mut stream = health_connection.stream.as_ref().lock().unwrap();
                            let update_message = Message::new(MessageType::InitConnection, None);

                            println!("Sending message to shard: {:?}", update_message);

                            let message_string = update_message.to_string();
                            stream.write_all(message_string.as_bytes()).unwrap();

                            println!("Waiting for response from shard");

                            let response: &mut [u8] = &mut [0; 1024];

                            // Wait for timeout and read response
                            stream.set_read_timeout(Some(std::time::Duration::new(10, 0))).unwrap();

                            match stream.read(response) { // TODO-SHARD: if node terminates, this will explode. Fix this!
                                Ok(_) => {
                                    let response_string = String::from_utf8_lossy(response);
                                    let response_message = Message::from_string(&response_string).unwrap();
                                    println!("Response from shard: {:?}", response_message);
                                    if response_message.message_type == MessageType::MemoryUpdate {
                                        println!(
                                            "{color_bright_green}Shard {} accepted the connection{style_reset}",
                                            node_port
                                        );
                                        let memory_size = response_message.payload.unwrap();
                                        println!("Memory size: {}", memory_size);
                                        shard_manager.add_shard(memory_size, node_port);
                                    } else {
                                        println!(
                                            "{color_red}Shard {} denied the connection{style_reset}",
                                            node_port
                                        );
                                    }
                                }
                                Err(_) => {
                                    println!(
                                        "{color_red}Shard {} did not respond{style_reset}",
                                        node_port
                                    );
                                }
                            }
                            
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
            shards: Arc::new(Mutex::new(shards)),
            shard_manager: Arc::new(shard_manager),
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

    fn get_shard_channel(node_ip: &str, node_port: &str) -> Result<Channel, io::Error> {
        let port = node_port.parse::<u64>().unwrap() + 1000;
        match TcpStream::connect(format!("{}:{}", node_ip, port)) {
            Ok(stream) => {
                println!(
                    "{color_bright_green}Health connection established with {}:{}{style_reset}",
                    node_ip, port
                );
                Ok(Channel { stream: Arc::new(Mutex::new(stream)) })
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

    /// Function that receives a query and checks for shards
    /// with corresponding data
    fn get_shards_for_query(&self, query: &str) -> (Vec<String>, bool) {
        // If it's an INSERT query return specific Shards
        if query_is_insert(query) {
            println!("Query is INSERT");

            let shard = self.shard_manager.peek().unwrap();
            println!("{color_bright_white}{style_bold}Returning shard: {}{style_reset}", shard);

            // TODO-SHARD: ask shard if it accepts insertions. If it does, pop it from the ShardManager and then update it.
            // If it doesn't, think of a way to handle this situation

            (vec![shard.clone()], true)
        } else {
            // Return all shards
            println!("{color_bright_white}{style_bold}Returning all shards{style_reset}");
            (self.shards.lock().unwrap().keys().cloned().collect(), false)
        }
    }

    fn ask_for_memory_update(&mut self, shard_id: String) {
        let comm_channels = self.comm_channels.read().unwrap();
        let shard_comm_channel = comm_channels.get(&shard_id).unwrap();
        let mut stream = shard_comm_channel.stream.as_ref().lock().unwrap();

        let message = Message::new(MessageType::AskMemoryUpdate, None);

        stream.write(message.to_string().as_bytes()).unwrap();

        let mut response: [u8; 1024] = [0; 1024];
        stream.read(&mut response).unwrap();

        let response_string = String::from_utf8_lossy(&response);
        let response_message = Message::from_string(&response_string).unwrap();

        if response_message.message_type == MessageType::MemoryUpdate {
            let memory_size = response_message.payload.unwrap();
            let mut shard_manager = self.shard_manager.as_ref().clone();
            shard_manager.update_shard_memory(&shard_id, memory_size);
        }
    }
}

impl NodeRole for Router {
    fn send_query(&mut self, query: &str) -> bool {
        println!("Router send_query called with query: {:?}", query);

        let (shards, is_insert) = self.get_shards_for_query(query);

        if shards.len() == 0 {
            eprintln!("No shards found for the query");
            return false;
        }

        for shard_id in shards {
            if let Some(mut shard) = self.clone().shards.lock().unwrap().get_mut(&shard_id) {
                let rows = match shard.query(query, &[]) {
                    Ok(rows) => {
                        println!("Query executed successfully: {:?}", rows);
                        rows
                    }
                    Err(e) => {
                        eprintln!("Failed to send the query to the shard: {:?}", e);
                        continue;
                    }
                };

                if is_insert {
                    self.ask_for_memory_update(shard_id);
                }

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
