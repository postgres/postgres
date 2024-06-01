use std::io::{Read, Write};
use std::net::TcpListener;

fn main() -> std::io::Result<()> {
    let listener = TcpListener::bind("127.0.0.1:7878")?;
    println!("Server listening in port 7878");

    for stream in listener.incoming() {
        match stream {
            Ok(mut stream) => {
                println!("New connection!: {}", stream.peer_addr()?);
                let mut buffer = [0; 512];
                stream.read(&mut buffer)?;
                println!("Received: {}", String::from_utf8_lossy(&buffer[..]));

                stream.write(b"Hello from server!")?;
            }
            Err(e) => {
                eprintln!("Error: {}", e);
            }
        }
    }
    Ok(())
}
