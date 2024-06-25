extern crate pgwire;
use pgwire::error::PgWireError;
use pgwire::messages::data::DataRow;
use pgwire::messages::simplequery::Query;
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::TcpStream;

#[tokio::main]
async fn main() -> Result<(), PgWireError> {
    let mut connection: TcpStream = TcpStream::connect("localhost:5433")?;

    // Send a simple message
    let mut message = Query {
        query: "SELECT * from employees;".to_string(),
    };
    connection.write_all(message.as_bytes()).await?;
    let mut empty_array: [u8; 512] = [0; 512];
    // Receive and decode the response
    let mut response = connection.read(&mut *empty_array)?;
    println!("{}", response.to_string());

    connection.close()?;

    Ok(())
}
