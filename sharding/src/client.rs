extern crate pgwire;

use pgwire::error::PgWireError;
use pgwire::messages::data::DataRow;
use pgwire::messages::simplequery::Query;
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::TcpStream;

#[tokio::main]
async fn main() -> Result<(), PgWireError> {
    let mut connection = TcpStream::connect("localhost:5432").await;
    // TODO: Jobs as a Shard
    Ok(())

}
