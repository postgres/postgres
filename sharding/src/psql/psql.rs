#![allow(non_upper_case_globals)]
#![allow(non_camel_case_types)]
#![allow(non_snake_case)]
use crate::psql::common_h_bindings::pg_result;
use postgres::{Client, NoTls};
use rust_decimal::prelude::Decimal;
use std::ffi::CStr;

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
    let mut client = Client::connect("host=127.0.0.1 user=ncontinanza dbname=template1", NoTls).unwrap();

    let rows = client.query(query, &[])?;
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
