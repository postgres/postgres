use futures::lock::Mutex;
use inline_colorization::*;
use postgres::{Client as PostgresClient, NoTls};
use std::io::{Read, Write};
use std::net::{TcpListener, TcpStream};
use std::sync::{Arc, RwLock};
use std::{io, thread};

extern crate users;
use super::memory_manager::MemoryManager;
use super::messages::message::{Message, MessageType};
use super::node::*;
use crate::node::shard;
use crate::utils::node_config::get_shard_config;
use users::get_current_username;

/// This struct represents the Shard node in the distributed system. It will communicate with the router
#[repr(C)]
#[derive(Clone)]
pub struct Shard {
    // router: Client,
    backend: Arc<Mutex<PostgresClient>>,
    ip: Arc<str>,
    port: Arc<str>,
    listener: Arc<RwLock<TcpListener>>,
    router_stream: Arc<RwLock<Option<TcpStream>>>,
    memory_manager: Arc<Mutex<MemoryManager>>,
}

impl Shard {
    /// Creates a new Shard node with the given port
    pub fn new(ip: &str, port: &str) -> Self {
        println!("Creating a new Shard node with port: {}", port);

        // get username dynamically
        let username = match get_current_username() {
            Some(username) => username.to_string_lossy().to_string(),
            None => panic!("Failed to get current username"),
        };
        println!("Username found: {:?}", username);
        println!("Connecting to the database with port: {}", port);

        let backend: PostgresClient = match PostgresClient::connect(
            format!(
                "host=127.0.0.1 port={} user={} dbname=template1",
                port, username
            )
            .as_str(),
            NoTls,
        ) {
            Ok(backend) => backend,
            Err(e) => {
                eprintln!("Failed to connect to the database: {:?}", e);
                panic!("Failed to connect to the database");
            }
        };
        let listener = Arc::new(RwLock::new(
            TcpListener::bind(format!("{}:{}", ip, port.parse::<u64>().unwrap() + 1000)).unwrap(),
        ));

        // Initialize memory manager
        let config = get_shard_config();
        let memory_threshold = config.memory_threshold;
        let memory_manager = MemoryManager::new(memory_threshold);

        println!(
            "{color_blue}[Shard] Available Memory: {:?} %{style_reset}",
            memory_manager.available_memory_perc
        );
        println!(
            "{color_blue}[Shard] Accepts Insertions: {:?}{style_reset}",
            memory_manager.accepts_insertions()
        );

        let stream = Arc::new(RwLock::new(None));
        let shard = Shard {
            // router: clients,
            backend: Arc::new(Mutex::new(backend)),
            ip: Arc::from(ip),
            port: Arc::from(port),
            listener: listener.clone(),
            router_stream: stream.clone(),
            memory_manager: Arc::new(Mutex::new(memory_manager)),
        };

        let listener_clone = listener.clone();
        let stream_clone = stream.clone();

        let mut shard_clone = shard.clone();
        let _handle = thread::spawn(move || {
            shard_clone.accept_connections(listener_clone, stream_clone);
        });

        shard
    }

    fn accept_connections(
        &mut self,
        listener: Arc<RwLock<TcpListener>>,
        rw_stream: Arc<RwLock<Option<TcpStream>>>,
    ) {
        let listener_guard = listener.read().unwrap();
        match listener_guard.accept() {
            Ok((stream, addr)) => {
                println!("New connection accepted from {}.", addr);
                // let mut stream_guard = rw_stream.write().unwrap();

                self.router_stream = Arc::new(RwLock::new(Some(stream)));

                // Start listening for incoming messages in a thread
                let mut shard_clone = self.clone();
                let _handle = thread::spawn(move || {
                    shard_clone.listen();
                });
            }
            Err(e) => {
                eprintln!("Failed to accept a connection: {}", e);
            }
        }
    }

    // Listen for incoming messages
    pub fn listen(&mut self) {
        println!("Listening for incoming messages");
        let self_clone = self.clone();
        let stream = self_clone.router_stream.read().unwrap();
        loop {
            // TODO-SHARD fix this multiple match statements so they're not nested
            match stream.as_ref() {
                Some(mut stream) => {
                    let mut buffer = [0; 1024];
                    let mut retries = 10;
                    match stream.read(&mut buffer) {
                        Ok(_) => {
                            let message_string = String::from_utf8_lossy(&buffer);
                            // println!("Received message: {}", message_string);
                            match self.get_response_message(&message_string) {
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
                None => {
                    // Do nothing
                }
            };
        }
    }

    fn get_response_message(&mut self, message: &str) -> Option<String> {
        let message = match Message::from_string(&message) {
            Ok(message) => message,
            Err(_e) => {
                // eprintln!("Failed to parse message: {:?}", e);
                return None;
            }
        };

        match message.message_type {
            MessageType::InitConnection => {
                println!("Received an InitConnection message");
                let response_string = self.get_agreed_connection();
                println!("Response created: {}", response_string);
                Some(response_string)
            }
            MessageType::AskMemoryUpdate => {
                println!("Received an AskMemoryUpdate message");
                let response_string = self.get_memory_update_message();
                println!("Response created: {}", response_string);
                Some(response_string)
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
        let response_message = shard::Message::new(MessageType::Agreed, Some(memory_percentage));

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
            shard::Message::new(MessageType::MemoryUpdate, Some(memory_percentage));

        response_message.to_string()
    }

    fn update(&mut self) -> Result<(), io::Error> {
        self.memory_manager.as_ref().try_lock().unwrap().update()
    }

    fn accepts_insertions(&self) -> bool {
        self.memory_manager
            .as_ref()
            .try_lock()
            .unwrap()
            .accepts_insertions()
    }

    fn get_insertion_response(&self) -> MessageType {
        if self.accepts_insertions() {
            MessageType::Agreed
        } else {
            MessageType::Denied
        }
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

    fn get_router_data(&self) -> (String, String) {
        (self.ip.to_string(), self.port.to_string())
    }
}
