use std::fmt::Debug;
use std::sync::Arc;

use async_trait::async_trait;
use futures::{stream, Sink, SinkExt, StreamExt};
use tokio::net::TcpListener;

use pgwire::api::auth::noop::NoopStartupHandler;
use pgwire::api::copy::NoopCopyHandler;
use pgwire::api::query::{PlaceholderExtendedQueryHandler, SimpleQueryHandler};
use pgwire::api::results::{DataRowEncoder, FieldFormat, FieldInfo, QueryResponse, Response, Tag};
use pgwire::api::{ClientInfo, PgWireHandlerFactory, Type};
use pgwire::error::ErrorInfo;
use pgwire::error::{PgWireError, PgWireResult};
use pgwire::messages::response::NoticeResponse;
use pgwire::messages::PgWireBackendMessage;
use pgwire::tokio::process_socket;

// Struct representing the connection with the Client
// Pgwire
pub struct Client;
pub struct Router {
    handler: Arc<Client>,
}

#[async_trait]
impl SimpleQueryHandler for Client {
    async fn do_query<'a, C>(
        &self,
        client: &mut C,
        query: &'a str,
    ) -> PgWireResult<Vec<Response<'a>>>
        where
            C: ClientInfo + Sink<PgWireBackendMessage> + Unpin + Send + Sync,
            C::Error: Debug,
            PgWireError: From<<C as Sink<PgWireBackendMessage>>::Error>,
    {
        client
            .send(PgWireBackendMessage::NoticeResponse(NoticeResponse::from(
                ErrorInfo::new(
                    "NOTICE".to_owned(),
                    "01000".to_owned(),
                    format!("Query received {}", query),
                ),
            )))
            .await?;

        if query.starts_with("SELECT") {
            let f1 = FieldInfo::new("id".into(), None, None, Type::INT4, FieldFormat::Text);
            let f2 = FieldInfo::new("name".into(), None, None, Type::VARCHAR, FieldFormat::Text);
            let schema = Arc::new(vec![f1, f2]);

            let data = vec![
                (Some(0), Some("Tom")),
                (Some(1), Some("Jerry")),
                (Some(2), None),
            ];
            let schema_ref = schema.clone();
            let data_row_stream = stream::iter(data.into_iter()).map(move |r| {
                let mut encoder = DataRowEncoder::new(schema_ref.clone());
                encoder.encode_field(&r.0)?;
                encoder.encode_field(&r.1)?;

                encoder.finish()
            });

            Ok(vec![Response::Query(QueryResponse::new(
                schema,
                data_row_stream,
            ))])
        } else {
            Ok(vec![Response::Execution(Tag::new("OK").with_rows(1))])
        }
    }
}
impl PgWireHandlerFactory for Router {
    type StartupHandler = NoopStartupHandler;
    type SimpleQueryHandler = Client;
    type ExtendedQueryHandler = PlaceholderExtendedQueryHandler;
    type CopyHandler = NoopCopyHandler;

    fn simple_query_handler(&self) -> Arc<Self::SimpleQueryHandler> {
        self.handler.clone()
    }

    fn extended_query_handler(&self) -> Arc<Self::ExtendedQueryHandler> {
        Arc::new(PlaceholderExtendedQueryHandler)
    }

    fn startup_handler(&self) -> Arc<Self::StartupHandler> {
        Arc::new(NoopStartupHandler)
    }

    fn copy_handler(&self) -> Arc<Self::CopyHandler> {
        Arc::new(NoopCopyHandler)
    }
}
#[tokio::main]
pub async fn main() {
    let leader = Arc::new(Router {
        handler: Arc::new(Client),
    });
    // The idea is: Client connect to leader
    let server_addr = "127.0.0.1:5432";
    let listener = TcpListener::bind(server_addr).await.unwrap();
    println!("Listening to clients {}", server_addr);
    loop {
        let incoming_socket = listener.accept().await.unwrap();
        let leader_ref = leader.clone();
        // A task per connection
        let _ = tokio::spawn(async move {
            process_socket(incoming_socket.0, None, leader_ref).await.expect("Error accepting client");
            // Client - PGwire -> Leader - pgwire -> shard (executes rust-postgres inside)
            println!("Client {:?} connected successfully", incoming_socket.1);
        }).await;
    }
}
