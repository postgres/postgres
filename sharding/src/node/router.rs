use postgres::{Client, NoTls};
extern crate users;
use crate::utils::queries::query_is_insert;

use super::super::utils::node_config::*;
use super::node::*;
use inline_colorization::*;
use regex::Regex;
use rust_decimal::Decimal;
use serde_yaml;
use std::collections::HashMap;
use std::sync::{Arc, RwLock};
use std::{
    fs, io,
    net::{SocketAddr, TcpListener, TcpStream},
    sync::Mutex,
    thread,
};
use users::get_current_username;
use crate::utils::node_config::get_router_config;

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
    comm_channels: Arc<RwLock<Vec<Channel>>>,
    ip: Arc<str>,
    port: Arc<str>,
}

impl Router {
    /// Creates a new Router node with the given port
    pub fn new(ip: &str, port: &str, config_path: Option<&str>) -> Self {
        
        let config = get_router_config(config_path);

        let mut shards: HashMap<String, Client> = HashMap::new();
        let mut comm_channels = Vec::new();
        for node in config.nodes {
            println!("Trying to connect to ip {} and port: {}", node.ip, node.port);
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
                            comm_channels.push(health_connection);
                        }
                        Err(e) => {
                            println!("Failed to connect to port: {}", node_port);
                        } // Do something here
                    }
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
                    "{color_red}Error establishing health connection with {}:{}{style_reset}",
                    node_ip, port
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
    pub fn get_shards_for_query(&self, query: &str) -> Vec<String> {
        // If it's an INSERT query return specific Shards
        if query_is_insert(query) {
            println!("Query is INSERT");

            // TODO-SHARD: Elegir un shard, el que tenga menor cargo o algo, etc
            return vec!["5433".to_string()];
        } else {
            // Return all shards
            println!("{color_bright_white}{style_bold}Returning all shards{style_reset}");
            self.shards.lock().unwrap().keys().cloned().collect()
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
            if let Some(shard) = self.shards.lock().unwrap().get_mut(&shard) {
                let rows = match shard.query(query, &[]) {
                    Ok(rows) => rows,
                    Err(e) => {
                        eprintln!("Failed to send the query to the shard: {:?}", e);
                        return false;
                    }
                };

                // TODO-SHARD: maybe this can be encapsulated inside another trait, with `.query` included
                // TODO-SHARD: Send Update Message to Shard somehow

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

    fn accepts_insertions(&self) -> bool {
        false
    }
}
