use inline_colorization::*;
use postgres::Client as PostgresClient;
use std::io::{Read, Write};
use std::net::{TcpListener, TcpStream};
use std::sync::{Arc, RwLock, Mutex};
use std::{io, thread};

extern crate users;
use super::memory_manager::MemoryManager;
use super::messages::message::{Message, MessageType, NodeInfo};
use super::node::*;
use crate::node::shard;
use crate::utils::common::connect_to_node;
use crate::utils::node_config::get_shard_config;

/// This struct represents the Shard node in the distributed system. It will communicate with the router
#[repr(C)]
#[derive(Clone)]
pub struct Shard {
    // router: Client,
    backend: Arc<Mutex<PostgresClient>>,
    ip: Arc<str>,
    port: Arc<str>,
    listener: Arc<TcpListener>,
    memory_manager: Arc<Mutex<MemoryManager>>,
    router_info: Arc<Mutex<Option<NodeInfo>>>
}

use std::fmt;
impl fmt::Debug for Shard {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        f.debug_struct("Shard")
            .field("ip", &self.ip)
            .field("port", &self.port)
            .field("router_info", &self.router_info)
            .finish()
    }
}

impl Shard {
    /// Creates a new Shard node with the given port
    pub fn new(ip: &str, port: &str) -> Self {
        println!("Creating a new Shard node with port: {}", port);
        println!("Connecting to the database with port: {}", port);

        let backend: PostgresClient = connect_to_node(ip, port).unwrap();
        let listener = Arc::new(
            TcpListener::bind(format!("{}:{}", ip, port.parse::<u64>().unwrap() + 1000)).unwrap(),
        );

        // Initialize memory manager
        let config = get_shard_config();
        let reserved_memory = config.unavailable_memory_perc;
        let memory_manager = MemoryManager::new(reserved_memory);

        println!(
            "{color_blue}[Shard] Available Memory: {:?} %{style_reset}",
            memory_manager.available_memory_perc
        );


        let shard = Shard {
            // router: clients,
            backend: Arc::new(Mutex::new(backend)),
            ip: Arc::from(ip),
            port: Arc::from(port),
            listener: listener.clone(),
            memory_manager: Arc::new(Mutex::new(memory_manager)),
            router_info: Arc::new(Mutex::new(None)),
        };
        shard
    }

    pub fn accept_connections(
        shared_shard: Arc<Mutex<Shard>>,
    ) {
        let shard = shared_shard.lock().unwrap();
        let listener = shard.listener.as_ref();
        loop {
            println!("Listening for incoming connections");
            match listener.accept() {
                Ok((mut stream, addr)) => {
                    println!(
                        "{color_bright_green}[SHARD1] New connection accepted from {}.{style_reset}",
                        addr
                    );

                    // Start listening for incoming messages in a thread
                    let shard_clone = shared_shard.clone();
                    
                    let _handle = thread::spawn(move || {
                        println!("[SHARD1] Inside listening thread");
                        Shard::listen(shard_clone, &mut stream);
                    });
                }
                Err(e) => {
                    eprintln!("Failed to accept a connection: {}", e);
                }
            }
        }
    }

    // Listen for incoming messages
    pub fn listen(shared_shard: Arc<Mutex<Shard>>, stream: &mut TcpStream) {
        println!("[LISTEN] Listening for incoming messages from stream: {:?}", stream);
        loop {
            let mut shard = shared_shard.lock().unwrap();
            let mut buffer = [0; 1024];
            let mut retries = 10;
            match stream.read(&mut buffer) {
                Ok(_) => {
                    let message_string = String::from_utf8_lossy(&buffer);
                    // println!("Received message: {}", message_string);
                    match shard.get_response_message(&message_string) {
                        Some(response) => {
                            stream.write(response.as_bytes()).unwrap();
                        }
                        None => {
                            retries -= 1;
                            if retries == 0 {
                                eprintln!("Stream has exceeded the number of retries");
                                break;
                            }
                        }
                    }
                }
                Err(e) => {
                    eprintln!("Failed to read from stream: {:?}", e);
                }
            }
        }
    }

    fn get_response_message(&mut self, message: &str) -> Option<String> {

        //print self
        println!("Self: {:?}", self);


        if message.is_empty() {
            return None;
        }

        let message = match Message::from_string(&message) {
            Ok(message) => message,
            Err(_e) => {
                // eprintln!("Failed to parse message: {:?}. Message: [{:?}]", e, message);
                return None;
            }
        };

        match message.message_type {
            MessageType::InitConnection => {
                let router_info = message.node_info.unwrap();
                self.router_info = Arc::new(Mutex::new(Some(router_info.clone())));
                println!("Router info: {:?}", self.router_info);
                println!("{color_bright_green}Received an InitConnection message{style_reset}");
                let response_string = self.get_agreed_connection();
                println!("Response created: {}", response_string);
                Some(response_string)
            }
            MessageType::AskMemoryUpdate => {
                println!("{color_bright_green}Received an AskMemoryUpdate message{style_reset}");
                let response_string = self.get_memory_update_message();
                println!("Response created: {}", response_string);
                Some(response_string)
            }
            MessageType::GetRouter => {
                println!("{color_bright_green}Received a GetRouter message{style_reset}");
                
                let self_clone = self.clone();
                let router_info: Option<NodeInfo> = {
                    let router_info = self_clone.router_info.as_ref().try_lock().unwrap();
                    router_info.clone()
                };

                println!("[SHARD RESP MSG] Router info: {:?}", router_info);
                match router_info {
                    Some(router_info) => {
                        let response_message =
                            Message::new(MessageType::RouterId, None, Some(router_info.clone()));
                        Some(response_message.to_string())
                    }
                    None => {
                        let response_message = Message::new(MessageType::NoRouterData, None, None);
                        Some(response_message.to_string())
                    }
                }
            }
            _ => {
                eprintln!(
                    "Message type received: {:?}, not yet implemented",
                    message.message_type
                );
                None
            }
        }
    }

    fn get_agreed_connection(&self) -> String {
        let memory_manager = self.memory_manager.as_ref().try_lock().unwrap();
        let memory_percentage = memory_manager.available_memory_perc;
        let response_message =
            shard::Message::new(MessageType::Agreed, Some(memory_percentage), None);

        response_message.to_string()
    }

    fn get_memory_update_message(&mut self) -> String {
        match self.update() {
            Ok(_) => {
                println!("Memory updated successfully");
            }
            Err(e) => {
                eprintln!("Failed to update memory: {:?}", e);
            }
        }
        let memory_manager = self.memory_manager.as_ref().try_lock().unwrap();
        let memory_percentage = memory_manager.available_memory_perc;
        let response_message =
            shard::Message::new(MessageType::MemoryUpdate, Some(memory_percentage), None);

        response_message.to_string()
    }

    fn update(&mut self) -> Result<(), io::Error> {
        self.memory_manager.as_ref().try_lock().unwrap().update()
    }
}

impl NodeRole for Shard {
    fn send_query(&mut self, query: &str) -> bool {
        match self.backend.as_ref().try_lock().unwrap().query(query, &[]) {
            Ok(rows) => rows,
            Err(e) => {
                eprintln!("Failed to execute query: {:?}", e);
                return false;
            }
        };

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

        true
    }
}
