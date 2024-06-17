extern crate pgwire;

use std::fs::{File, OpenOptions};
use std::io::Read;
use std::net::TcpStream;
use pgwire::error::{PgWireError};
use pgwire::api::{PgWireConnectionState, ClientInfo};
use std::net::TcpListener;
use tokio::io::AsyncWriteExt;
use sharding::pg_result;

mod libpqint;
extern "C" {
    fn PSQLexec(query: *char) -> pg_result;
}

fn main() -> Result<(), PgWireError> {
    // Start the pgwire server on port 5432
    let server = TcpListener::bind("127.0.0.1:5433")?;

    // Handle incoming connections
    for connection in server.incoming() {
        println!("Client connected from {:?}", connection.addr());
        let mut empty_array: [u8; 512] = [0; 512];
        let _ = connection.unwrap().read(&mut *empty_array);
        let mut file = OpenOptions::new()
            .read(true)
            .write(true)
            .create(true)
            .open("../../src/bin/psql/testing.txt");

        // Write the buffer to the file
        file.write_all(&mut *empty_array)?;

        // Flush the data to ensure it's written to disk
        file.flush()?;
    }

    Ok(())
}
