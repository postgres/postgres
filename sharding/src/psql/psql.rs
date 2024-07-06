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
pub extern "C" fn SendQueryToShard(query_data: *const i8) {
    println!("SendQueryToShard called");
    unsafe {
        if query_data.is_null() {
            eprintln!("Received a null pointer");
            return;
        }

        let c_str = CStr::from_ptr(query_data);
        let query = match c_str.to_str() {
            Ok(str) => str,
            Err(_) => {
                eprintln!("Received an invalid UTF-8 string");
                return;
            }
        };

        println!("Received Query: {:?}", query);
        let _ = handle_query(query.trim());
    }
}

fn handle_query(query: &str) -> Result<(), Box<dyn std::error::Error>> {
    // get username dynamically
    let username = match get_current_username() {
        Some(username) => username.to_string_lossy().to_string(),
        None => panic!("Failed to get current username"),
    };
    println!("Username found: {:?}", username);
    
    let mut client = Client::connect(format!("host=127.0.0.1 user={} dbname=template1", username).as_str(), NoTls).unwrap();

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
