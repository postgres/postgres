use std::net::SocketAddr;
use std::sync::{Arc, Mutex};
use std::io::Read;
use tokio::io::AsyncReadExt;
use std::net::{TcpListener, TcpStream};

// Struct representing the connection with the Client
pub struct Client {
    stream: TcpStream,
    n_queries: u16,
}

pub struct Router {
    clients_connected: Mutex<Vec<Client>>,
}

impl Router {
    fn new() -> Self {
        Router {
            clients_connected: Mutex::new(Vec::new()),
        }
    }

    fn add_client(&self, client: TcpStream) {
        let client = Client {
            stream: client,
            n_queries: 0,
        };
        self.clients_connected.lock().unwrap().push(client);
    }

    async fn handle_client(&self, mut client: TcpStream, addr: SocketAddr) {
        println!("Handling client from {}", addr);
        println!("Connections: {}", self.clients_connected.lock().unwrap().len());
        let mut buf = [0; 1024];
        match client.read(&mut buf) {
            Ok(n) if n == 0 => {
                // Connection was closed
                println!("Client from {} disconnected", addr);
            }
            Ok(n) => {
                if let Ok(data) = String::from_utf8(buf[..n].to_vec()) {
                    println!("Received data from {}: {}", addr, data);
                } else {
                    println!("Received invalid UTF-8 data from {}", addr);
                }
            }
            Err(e) => {
                println!("Error reading from {}: {:?}", addr, e);
            }
        }
    }
}

#[tokio::main]
pub async fn main() {
    let leader = Arc::new(Router::new());
    // The idea is: Client connect to leader
    let server_addr = "127.0.0.1:10000";
    let listener = TcpListener::bind(server_addr).unwrap();
    println!("Listening to clients {}", server_addr);

    loop {
        let (incoming_socket, addr) = listener.accept().unwrap();
        // Store every connection in leader
        let leader_ref = leader.clone();
        leader_ref.add_client(incoming_socket.try_clone().expect("Error cloning socket"));

        // A task per connection
        tokio::spawn(async move {
            leader_ref.handle_client(incoming_socket, addr).await;
            println!("Client {} connected successfully", addr);
        });
    }
}
