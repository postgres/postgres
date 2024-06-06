
#![allow(non_upper_case_globals)]
#![allow(non_camel_case_types)]
#![allow(non_snake_case)]

use std::ffi::CStr;

mod bindings;include!("bindings.rs");

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

        //println!("Query: {}", query.trim());
    }
}