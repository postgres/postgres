#![allow(non_upper_case_globals)]
#![allow(non_camel_case_types)]
#![allow(non_snake_case)]

use std::ffi::CStr;
use std::io::{Read, Write};
use std::net::{TcpListener, TcpStream};
use std::sync::{Arc, Mutex};
use std::thread;
use tokio::runtime::{Builder, Runtime};
use tokio::sync::oneshot;

use tokio::spawn;

use crate::psql::common_h_bindings::pg_result;

#[no_mangle]
pub extern "C" fn SendQueryToShard(query_data: *const i8) {
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

        println!("Query: {}", query.trim());
    }
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
