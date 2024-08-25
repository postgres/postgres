use postgres::{Client as PostgresClient, NoTls};
extern crate users;
use crate::node::messages::message::{Message, MessageType, NodeInfo};
use crate::utils::queries::query_is_insert;

use super::node::*;
use super::shard_manager::ShardManager;
use crate::utils::common::get_username_dinamically;
use crate::utils::node_config::{get_router_config, Node};
use inline_colorization::*;
use rust_decimal::Decimal;
use std::collections::HashMap;
use std::io::{Read, Write};
use std::sync::{Arc, RwLock};
use std::{io, net::TcpStream, sync::Mutex};

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
    shards: Arc<Mutex<HashMap<String, PostgresClient>>>,
    shard_manager: Arc<ShardManager>,
    ///  HashMap:
    ///     key: Hash
    ///     value: shardId
    comm_channels: Arc<RwLock<HashMap<String, Channel>>>,
    ip: Arc<str>,
    port: Arc<str>,
}

impl Router {
    /// Creates a new Router node with the given port and ip, connecting it to the shards specified in the configuration file.
    pub fn new(ip: &str, port: &str, config_path: Option<&str>) -> Self {
        Router::initialize_router_with_connections(ip, port, config_path)
    }

    /// Initializes the Router node with connections to the shards specified in the configuration file.
    fn initialize_router_with_connections(
        ip: &str,
        port: &str,
        config_path: Option<&str>,
    ) -> Router {
        let config = get_router_config(config_path);
        let shards: HashMap<String, PostgresClient> = HashMap::new();
        let comm_channels: HashMap<String, Channel> = HashMap::new();
        let shard_manager = ShardManager::new();

        let mut router = Router {
            shards: Arc::new(Mutex::new(shards)),
            shard_manager: Arc::new(shard_manager),
            comm_channels: Arc::new(RwLock::new(comm_channels)),
            ip: Arc::from(ip),
            port: Arc::from(port),
        };

        for shard in config.nodes {
            if (shard.ip == router.ip.as_ref()) && (shard.port == router.port.as_ref()) {
                continue;
            }
            router.configure_shard_connection_to(shard)
        }
        router
    }

    /// Configures the connection to a shard with the given ip and port.
    fn configure_shard_connection_to(&mut self, node: Node) {
        let node_ip = node.ip;
        let node_port = node.port;

        let shard_client = match Router::connect_to_node(&node_ip, &node_port) {
            Ok(shard_client) => shard_client,
            Err(_) => {
                println!("Failed to connect to port: {}", node_port);
                return;
            }
        };
        println!("Connected to ip {} and port: {}", node_ip, node_port);

        self.save_shard_client(node_port.to_string(), shard_client);
        self.set_health_connection(node_ip.as_str(), node_port.as_str());
    }

    /// Saves the shard client in the Router's shards HashMap with its corresponding shard id as key.
    fn save_shard_client(&mut self, shard_id: String, shard_client: PostgresClient) {
        let mut shards = self.shards.lock().unwrap();
        shards.insert(shard_id, shard_client);
    }

    /// Sets the health_connection to the shard with the given ip and port, initializing the communication with a handshake between the router and the shard.
    fn set_health_connection(&mut self, node_ip: &str, node_port: &str) {
        let health_connection = match Router::get_shard_channel(&node_ip, &node_port) {
            Ok(health_connection) => health_connection,
            Err(_) => {
                println!("Failed to create health-connection to port: {}", node_port);
                return;
            }
        };
        if self.send_init_connection_message(health_connection.clone(), node_port) {
            self.save_comm_channel(node_port.to_string(), health_connection);
        }
    }

    /// Saves the communication channel to the shard with the given shard id as key.
    fn save_comm_channel(&mut self, shard_id: String, channel: Channel) {
        let mut comm_channels = self.comm_channels.write().unwrap();
        comm_channels.insert(shard_id, channel);
    }

    /// Sends the InitConnection message to the shard with the given shard id, initializing the communication with a handshake between the router and the shard. The shard will respond with a MemoryUpdate message, which will be handled by the router updating the shard's memory size in the ShardManager.
    fn send_init_connection_message(
        &mut self,
        health_connection: Channel,
        node_port: &str,
    ) -> bool {
        // Send InitConnection Message to Shard and save shard to ShardManager
        let mut stream = health_connection.stream.as_ref().lock().unwrap();
        let update_message = Message::new(
            MessageType::InitConnection,
            None,
            Some(NodeInfo {
                ip: self.ip.as_ref().to_string(),
                port: self.port.as_ref().to_string(),
            }),
        );

        println!("Sending message to shard: {:?}", update_message);

        let message_string = update_message.to_string();
        stream.write_all(message_string.as_bytes()).unwrap();

        println!("Waiting for response from shard");

        let response: &mut [u8] = &mut [0; 1024];

        // Wait for timeout and read response
        stream
            .set_read_timeout(Some(std::time::Duration::new(10, 0)))
            .unwrap();

        match stream.read(response) {
            Ok(_) => {
                let response_string = String::from_utf8_lossy(response);
                let response_message = Message::from_string(&response_string).unwrap();
                println!("Response from shard: {:?}", response_message);
                self.handle_response(response_message, node_port)
            }
            Err(_) => {
                println!(
                    "{color_red}Shard {} did not respond{style_reset}",
                    node_port
                );
                false
            }
        }
    }

    /// Handles the responses from the shard from the health_connection channel.
    fn handle_response(&mut self, response_message: Message, node_port: &str) -> bool {
        match response_message.message_type {
            MessageType::Agreed => {
                println!(
                    "{color_bright_green}Shard {} accepted the connection{style_reset}",
                    node_port
                );
                let memory_size = response_message.payload.unwrap();
                println!("Memory size: {}", memory_size);
                self.save_shard_in_manager(memory_size, node_port.to_string());
                true
            }
            MessageType::MemoryUpdate => {
                let memory_size = response_message.payload.unwrap();
                println!(
                    "{color_bright_green}Shard {} updated its memory size to {}{style_reset}",
                    node_port, memory_size
                );
                self.update_shard_in_manager(memory_size, node_port.to_string());
                true
            }
            _ => {
                println!(
                    "{color_red}Shard {} denied the connection{style_reset}",
                    node_port
                );
                false
            }
        }
    }

    /// Adds a shard to the ShardManager with the given memory size and shard id.
    fn save_shard_in_manager(&mut self, memory_size: f64, shard_id: String) {
        let mut shard_manager = self.shard_manager.as_ref().clone();
        shard_manager.add_shard(memory_size, shard_id.clone());
        println!(
            "{color_bright_green}Shard {} added to ShardManager{style_reset}",
            shard_id
        );
    }

    /// Updates the shard in the ShardManager with the given memory size and shard id.
    fn update_shard_in_manager(&mut self, memory_size: f64, shard_id: String) {
        let mut shard_manager = self.shard_manager.as_ref().clone();
        shard_manager.update_shard_memory(memory_size, shard_id.clone());
        println!(
            "{color_bright_green}Shard {} updated in ShardManager{style_reset}",
            shard_id
        );
    }

    /// Connects to the node with the given ip and port, returning a Client.
    fn connect_to_node(ip: &str, port: &str) -> Result<PostgresClient, postgres::Error> {
        // get username dynamically
        let username = get_username_dinamically();

        match PostgresClient::connect(
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

    /// Establishes a health connection with the node with the given ip and port, returning a Channel.
    fn get_shard_channel(node_ip: &str, node_port: &str) -> Result<Channel, io::Error> {
        let port = node_port.parse::<u64>().unwrap() + 1000;
        match TcpStream::connect(format!("{}:{}", node_ip, port)) {
            Ok(stream) => {
                println!(
                    "{color_bright_green}Health connection established with {}:{}{style_reset}",
                    node_ip, port
                );
                Ok(Channel {
                    stream: Arc::new(Mutex::new(stream)),
                })
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
        // If it's an INSERT query, return specific Shards.
        if query_is_insert(query) {
            println!("Query is INSERT");

            let shard = self.shard_manager.peek().unwrap();
            println!(
                "{color_bright_white}{style_bold}Returning shard: {}{style_reset}",
                shard
            );

            // TODO-SHARD: ask shard if it accepts insertions. If it does, pop it from the ShardManager and then update it.
            // If it doesn't, think of a way to handle this situation

            (vec![shard.clone()], true)
        } else {
            // Return all shards
            println!("{color_bright_white}{style_bold}Returning all shards{style_reset}");
            (self.shards.lock().unwrap().keys().cloned().collect(), false)
        }
    }

    /// Function that sends a message to the shard asking for a memory update. This must be called each time an insertion query is sent, and may be used to update the shard's memory size in the ShardManager in other circumstances.
    fn ask_for_memory_update(&mut self, shard_id: String) {
        // Get channel
        let self_clone = self.clone();
        let comm_channels = self_clone.comm_channels.read().unwrap();
        let shard_comm_channel = comm_channels.get(&shard_id).unwrap();
        let mut stream = shard_comm_channel.stream.as_ref().lock().unwrap();

        // Write message
        let message = Message::new(MessageType::AskMemoryUpdate, None, None);
        stream.write(message.to_string().as_bytes()).unwrap();
        let mut response: [u8; 1024] = [0; 1024];

        // Readn and handle message
        stream.read(&mut response).unwrap();
        let response_string = String::from_utf8_lossy(&response);
        let response_message = match Message::from_string(&response_string) {
            Ok(message) => message,
            Err(_) => {
                eprintln!("Failed to parse message from shard");
                // TODO-SHARD: handle this situation, should this try again? What happens if we can't update the shard's memory in the shard_manager?
                return;
            }
        };
        self.handle_response(response_message, shard_id.as_str());
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
            self.send_query_to_shard(shard_id, query, is_insert);
        }
        true
    }
}

impl NetworkNode for Router {
    fn get_router_data(&self) -> (String, String) {
        (self.ip.as_ref().to_string(), self.port.as_ref().to_string())
    }
}

impl Router {
    fn send_query_to_shard(&mut self, shard_id: String, query: &str, is_insert: bool) {
        if let Some(shard) = self.clone().shards.lock().unwrap().get_mut(&shard_id) {
            let rows = match shard.query(query, &[]) {
                Ok(rows) => rows,
                Err(e) => {
                    eprintln!("Failed to send the query to the shard: {:?}", e);
                    return;
                }
            };

            if is_insert {
                self.ask_for_memory_update(shard_id);
            }

            // TODO-SHARD: maybe this can be encapsulated inside another trait, with `.query` included

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
            eprintln!("Shard {:?} not found", shard_id);
            return;
        }
    }
}
