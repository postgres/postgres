
#![allow(non_upper_case_globals)]
#![allow(non_camel_case_types)]
#![allow(non_snake_case)]

use std::ffi::CStr;
use std::io::{Read, Write};
use std::net::{TcpListener, TcpStream};
use std::sync::{Arc, Mutex};
use std::thread;
use lazy_static::lazy_static;
use tokio::runtime::{Builder, Runtime};
use tokio::sync::oneshot;

use tokio::spawn;
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

        println!("Query: {}", query.trim());
    }
}
#[no_mangle]
pub extern "C" fn SendPGResultToShard(pg_result: *const pg_result) {
    unsafe {
        if pg_result.is_null() {
            eprintln!("Received a null pointer");
            return;
        }
        else {
            println!("Received PGResult!!!!");
            let n_tup = (*pg_result).ntups;
            println!("{:?}", n_tup);
        };
    }
}
lazy_static! {
    static ref RUNTIME: Arc<Mutex<Option<Runtime>>> = Arc::new(Mutex::new(None));
    static ref SHUTDOWN_SENDER: Arc<Mutex<Option<oneshot::Sender<()>>>> = Arc::new(Mutex::new(None));
}

#[no_mangle]
pub extern "C" fn handle_client() {
    let runtime = Builder::new_multi_thread()
        .worker_threads(4)
        .enable_all()
        .build()
        .expect("Failed to create runtime");

    let (tx, rx) = oneshot::channel();

    {
        let mut runtime_lock = RUNTIME.lock().unwrap();
        *runtime_lock = Some(runtime);

        let mut shutdown_sender_lock = SHUTDOWN_SENDER.lock().unwrap();
        *shutdown_sender_lock = Some(tx);
    }

    thread::spawn(move || {
        let binding = RUNTIME.lock().unwrap();
        let runtime = binding.as_ref().unwrap().clone();
        runtime.block_on(async move {
            let listener = TcpListener::bind("127.0.0.1:7878").expect("Could not bind to address");
            println!("Server listening on port 7878");

            tokio::select! {
                _ = async {
                    loop {
                        match listener.accept() {
                            Ok((stream, addr)) => {
                                println!("New connection: {}", addr);
                                tokio::spawn(handle_connection(stream));
                            }
                            Err(e) => {
                                eprintln!("Failed to accept connection: {}", e);
                            }
                        }
                    }
                } => {}
                _ = rx => {
                    println!("Shutting down server");
                }
            }
        });
    });
}

#[no_mangle]
pub extern "C" fn stop_server() {
    if let Some(sender) = SHUTDOWN_SENDER.lock().unwrap().take() {
        let _ = sender.send(());
    }

    let _ = RUNTIME.lock().unwrap().take();
}

async fn handle_connection(mut stream: TcpStream) {
    let mut buffer = [0; 512];
    loop {
        match stream.read(&mut buffer) {
            Ok(0) => {
                // Connection was closed
                break;
            }
            Ok(n) => {
                // Echo the data back to the client
                if let Err(e) = stream.write_all(&buffer[0..n]) {
                    eprintln!("Failed to send response: {}", e);
                    break;
                }
            }
            Err(e) => {
                eprintln!("Failed to read from connection: {}", e);
                break;
            }
        }
    }

    println!("Connection closed: {}", stream.peer_addr().unwrap());
}