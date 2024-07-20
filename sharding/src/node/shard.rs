use postgres::{Client, NoTls};
extern crate users;
use rust_decimal::Decimal;
use users::get_current_username;
use super::node::*;

/// This struct represents the Shard node in the distributed system. It will communicate with the router
#[repr(C)]
pub struct Shard<'a> {
    // router: Client,
    backend: Client,
    port: &'a str,
    // TODO-SHARD: add an attribute for the shard's network
}

impl<'a> Shard<'a> {
    /// Creates a new Shard node with the given port
    pub fn new(port: &'a str) -> Self {
        println!("Creating a new Shard node with port: {}", port);

        // get username dynamically
        let username = match get_current_username() {
            Some(username) => username.to_string_lossy().to_string(),
            None => panic!("Failed to get current username"),
        };
        println!("Username found: {:?}", username);
        println!("Connecting to the database with port: {}", port);

        let mut backend: Client = match Client::connect(
            format!("host=127.0.0.1 port={} user={} dbname=template1", port, username).as_str(),
            NoTls,
        ) {
            Ok(backend) => backend,
            Err(e) => {
                eprintln!("Failed to connect to the database: {:?}", e);
                panic!("Failed to connect to the database");
            }
        };


        Shard {
            // router: clients,
            backend,
            port
        }
    }
}

impl<'a> NodeRole for Shard<'a> {
    fn send_query(&mut self, query: &str) -> bool {
        
        let rows = match self.backend.query(query, &[]) {
            Ok(rows) => rows,
            Err(e) => {
                eprintln!("Failed to execute query: {:?}", e);
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
