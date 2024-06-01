
#![allow(non_upper_case_globals)]
#![allow(non_camel_case_types)]
#![allow(non_snake_case)]

mod bindings;include!("bindings.rs");

#[no_mangle]
pub extern "C" fn SendQueryToShard(query_data: *const i8) {
    println!("Query: {:?}", query_data);
}
