use super::memory_manager::MemoryManager;
use super::messages::message::{Message, MessageType};
use super::messages::node_info::NodeInfo;
use super::node::NodeRole;
use super::tables_id_info::TablesIdInfo;
use crate::node::shard;
use crate::utils::common::{connect_to_node, ConvertToString};
use crate::utils::node_config::get_memory_config;
use crate::utils::queries::print_rows;
use indexmap::IndexMap;
use inline_colorization::{color_blue, color_bright_green, style_reset};
use log::{debug, error, info};
use postgres::{Client as PostgresClient, Row};
use std::io::{Read, Write};
use std::net::{TcpListener, TcpStream};
use std::sync::{Arc, Mutex};
use std::{io, thread};

extern crate users;

/// This struct represents the Shard node in the distributed system. It will communicate with the router
#[repr(C)]
#[derive(Clone)]
pub struct Shard {
    backend: Arc<Mutex<PostgresClient>>,
    ip: Arc<str>,
    port: Arc<str>,
    memory_manager: Arc<Mutex<MemoryManager>>,
    router_info: Arc<Mutex<Option<NodeInfo>>>,
    tables_max_id: Arc<Mutex<TablesIdInfo>>,
}

use std::fmt;
impl fmt::Debug for Shard {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        f.debug_struct("Shard")
            .field("ip", &self.ip)
            .field("port", &self.port)
            .field("router_info", &self.router_info)
            .field("tables_max_id", &self.tables_max_id)
            .finish()
    }
}

impl Shard {
    /// Creates a new Shard node in the given port.
    #[must_use]
    pub fn new(ip: &str, port: &str) -> Self {
        info!("Creating a new Shard node in port: {port}");
        info!("Connecting to the database in port: {port}");

        let backend: PostgresClient = connect_to_node(ip, port).unwrap();

        let memory_manager = Self::initialize_memory_manager();

        debug!(
            "{color_blue}[Shard] Available Memory: {:?} %{style_reset}",
            memory_manager.available_memory_perc
        );

        let mut shard = Shard {
            backend: Arc::new(Mutex::new(backend)),
            ip: Arc::from(ip),
            port: Arc::from(port),
            memory_manager: Arc::new(Mutex::new(memory_manager)),
            router_info: Arc::new(Mutex::new(None)),
            tables_max_id: Arc::new(Mutex::new(IndexMap::new())),
        };

        let _ = shard.update();

        info!("{color_bright_green}Shard created successfully. Shard: {shard:?}{style_reset}");

        shard
    }

    fn initialize_memory_manager() -> MemoryManager {
        let config = get_memory_config();
        let reserved_memory = config.unavailable_memory_perc;
        MemoryManager::new(reserved_memory)
    }

    pub fn accept_connections(shared_shard: &Arc<Mutex<Shard>>, ip: &str, port: &str) {
        let listener =
            TcpListener::bind(format!("{}:{}", ip, port.parse::<u64>().unwrap() + 1000)).unwrap();

        loop {
            match listener.accept() {
                Ok((stream, addr)) => {
                    info!(
                        "{color_bright_green}[SHARD] New connection accepted from {addr}.{style_reset}",
                    );

                    // Start listening for incoming messages in a thread
                    let shard_clone = shared_shard.clone();
                    let shareable_stream = Arc::new(Mutex::new(stream));
                    let stream_clone = Arc::clone(&shareable_stream);

                    let _handle = thread::spawn(move || {
                        Shard::listen(&shard_clone, &stream_clone);
                    });
                }
                Err(e) => {
                    error!("Failed to accept a connection: {e}");
                }
            }
        }
    }

    // Listen for incoming messages
    pub fn listen(shared_shard: &Arc<Mutex<Shard>>, stream: &Arc<Mutex<TcpStream>>) {
        loop {
            // sleep for 1 millisecond to allow the stream to be ready to read
            thread::sleep(std::time::Duration::from_millis(1));
            let mut shard = shared_shard.lock().unwrap();
            let mut buffer = [0; 1024];

            let mut stream = stream.lock().unwrap();

            match stream.set_read_timeout(Some(std::time::Duration::new(10, 0))) {
                Ok(()) => {}
                Err(_e) => {
                    continue;
                }
            }

            match stream.read(&mut buffer) {
                Ok(chars) => {
                    if chars == 0 {
                        continue;
                    }

                    let message_string = String::from_utf8_lossy(&buffer);

                    if let Some(response) = shard.get_response_message(&message_string) {
                        debug!("{color_bright_green}Sending response: {response}{style_reset}");
                        // stream.write(response.as_bytes()).unwrap();
                        stream.write_all(response.as_bytes()).unwrap();
                    } else {
                        // do nothing
                    }
                }
                Err(_e) => {
                    // could not read from the stream, ignore
                }
            }
        }
    }

    fn get_response_message(&mut self, message: &str) -> Option<String> {
        if message.is_empty() {
            return None;
        }

        let message = match Message::from_string(message) {
            Ok(message) => message,
            Err(e) => {
                error!("Failed to parse message: {e:?}. Message: [{message:?}]");
                return None;
            }
        };

        match message.get_message_type() {
            MessageType::InitConnection => {
                self.handle_init_connection_message(message)
            }
            MessageType::AskMemoryUpdate => {
                self.handle_memory_update_message()
            }
            MessageType::GetRouter => {
                self.handle_get_router_message()
            }
            _ => {
                error!(
                    "Message type received: {:?}, not yet implemented",
                    message.get_message_type()
                );
                None
            }
        }
    }

    fn handle_init_connection_message(&mut self, message: Message) -> Option<String> {
        let router_info = message.get_data().node_info.unwrap();
        self.router_info = Arc::new(Mutex::new(Some(router_info.clone())));
        debug!("{color_bright_green}Received an InitConnection message{style_reset}");
        let response_string = self.get_agreed_connection();
        Some(response_string)
    }

    fn handle_memory_update_message(&mut self) -> Option<String> {
        debug!("{color_bright_green}Received an AskMemoryUpdate message{style_reset}");
        let response_string = self.get_memory_update_message();
        Some(response_string)
    }

    fn handle_get_router_message(&mut self) -> Option<String> {
        debug!("{color_bright_green}Received a GetRouter message{style_reset}");
        let self_clone = self.clone();
        let router_info: Option<NodeInfo> = {
            let router_info = self_clone.router_info.as_ref().try_lock().unwrap();
            router_info.clone()
        };

        if let Some(router_info) = router_info {
            let response_message = Message::new_router_id(router_info.clone());
            Some(response_message.to_string())
        } else {
            let response_message = Message::new_no_router_data();
            Some(response_message.to_string())
        }
    }

    fn get_agreed_connection(&self) -> String {
        let memory_manager = self.memory_manager.as_ref().try_lock().unwrap();
        let memory_percentage = memory_manager.available_memory_perc;
        let tables_max_id_clone = self.tables_max_id.as_ref().try_lock().unwrap().clone();
        let response_message = shard::Message::new_agreed(memory_percentage, tables_max_id_clone);

        response_message.to_string()
    }

    fn get_memory_update_message(&mut self) -> String {
        match self.update() {
            Ok(()) => {
                debug!("Memory updated successfully");
            }
            Err(e) => {
                error!("Failed to update memory: {e:?}");
            }
        }
        let memory_manager = self.memory_manager.as_ref().try_lock().unwrap();
        let memory_percentage = memory_manager.available_memory_perc;
        let tables_max_id_clone = self.tables_max_id.as_ref().try_lock().unwrap().clone();
        let response_message =
            shard::Message::new_memory_update(memory_percentage, tables_max_id_clone);

        response_message.to_string()
    }

    fn update(&mut self) -> Result<(), io::Error> {
        self.set_max_ids();
        self.memory_manager.as_ref().try_lock().unwrap().update()
    }

    fn get_all_tables(&mut self) -> Vec<String> {
        let query =
            "SELECT table_name FROM information_schema.tables WHERE table_schema = 'public'";
        let Some(rows) = self.get_rows_for_query(query) else {
            return Vec::new();
        };
        let mut tables = Vec::new();
        for row in rows {
            let table_name: String = row.get(0);
            tables.push(table_name);
        }
        tables
    }

    // Set the max ids for all tables in tables_max_id
    fn set_max_ids(&mut self) {
        let tables = self.get_all_tables();
        for table in tables {
            let query = format!("SELECT MAX(id) FROM {table}");
            if let Some(rows) = self.get_rows_for_query(&query) {
                let max_id: i32 = if let Ok(id) = rows[0].try_get(0) {
                    id
                } else {
                    error!("Failed to get max id for table: {table}. Table might be empty",);
                    0
                };
                let mut tables_max_id = self.tables_max_id.as_ref().try_lock().unwrap();
                tables_max_id.insert(table, i64::from(max_id));
            }
        }
    }

    fn get_rows_for_query(&mut self, query: &str) -> Option<Vec<Row>> {
        match self.backend.as_ref().try_lock().unwrap().query(query, &[]) {
            Ok(rows) => {
                if rows.is_empty() {
                    return None;
                }
                print_rows(rows.clone());
                Some(rows)
            }
            Err(e) => {
                error!("Failed to execute query: {e:?}");
                None
            }
        }
    }
}

impl NodeRole for Shard {
    fn send_query(&mut self, query: &str) -> Option<String> {
        debug!("{color_bright_green}Sending query to the database: {query}{style_reset}");
        let rows = self.get_rows_for_query(query)?;
        let _ = self.update(); // Updates memory and tables_max_id
        Some(rows.convert_to_string())
    }
}
