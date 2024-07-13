#![allow(non_upper_case_globals)]
#![allow(non_camel_case_types)]
#![allow(non_snake_case)]
use crate::psql::common_h_bindings::pg_result;
use postgres::{Client, NoTls};
use rust_decimal::prelude::Decimal;
use std::ffi::CStr;
extern crate users;
use users::get_current_username;

#[no_mangle]
pub extern "C" fn SendQueryToShard(query_data: *const i8) -> bool {
    println!("SendQueryToShard called");
    unsafe {
        if query_data.is_null() {
            eprintln!("Received a null pointer");
            return false;
        }

        let c_str = CStr::from_ptr(query_data);
        let query = match c_str.to_str() {
            Ok(str) => str,
            Err(_) => {
                eprintln!("Received an invalid UTF-8 string");
                return false;
            }
        };

        println!("Received Query: {:?}", query);
        let query_result = handle_query(query.trim());
        match query_result {
            Ok(_) => {
                println!("Query executed successfully");
                true
            }
            Err(e) => {
                eprintln!("Failed to execute query: {:?}", e);
                false
            }
        }

    }
}

fn handle_query(query: &str) -> Result<(), Box<dyn std::error::Error>> {
    // get username dynamically
    let username = match get_current_username() {
        Some(username) => username.to_string_lossy().to_string(),
        None => panic!("Failed to get current username"),
    };
    println!("Username found: {:?}", username);
    
    // TODO-SHARD: port needs to be dynamic
    let mut client: Client = match Client::connect(format!("host=127.0.0.1 port=5433 user={} dbname=template1", username).as_str(), NoTls) {
        Ok(client) => client,
        Err(e) => {
            eprintln!("Failed to connect to the database: {:?}", e);
            panic!("Failed to connect to the database");
        }
    };

    let rows = client.query(query, &[])?;
    println!("{:?}", rows);
    Ok(())
}

#[no_mangle]
pub extern "C" fn SendPGResultToShard(pg_result: *const pg_result) {
    unsafe {
        if pg_result.is_null() {
            eprintln!("Received a null pointer");
            return;
        } else {
            let n_tup = (*pg_result).ntups;
            println!("{:?}", n_tup);
        };
    }
}
