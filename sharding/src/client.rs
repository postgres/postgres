use tokio::io::{self, AsyncReadExt, AsyncWriteExt};
use tokio::net::TcpStream;

#[tokio::main]
async fn main() -> io::Result<()> {
    let mut stream = TcpStream::connect("127.0.0.1:7878").await?;
    println!("Connected to the server");

    // Send a message to the server
    let message = b"Hello, server!";
    stream.write_all(message).await?;
    println!("Sent: {}", String::from_utf8_lossy(message));

    // Buffer to hold the response from the server
    let mut buffer = vec![0; 512];

    // Read the server's response
    let n = stream.read(&mut buffer).await?;
    println!("Received: {}", String::from_utf8_lossy(&buffer[..n]));

    Ok(())
}
