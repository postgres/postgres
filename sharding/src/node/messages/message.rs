use std::{fmt, str::FromStr};

use crate::node;

/// MessageType enum shows which command is being sent
#[derive(Debug, PartialEq, Clone)]
pub enum MessageType {
    InitConnection,
    AskMemoryUpdate,
    MemoryUpdate,
    AskInsertionAcceptance,
    Agreed,
    Denied,
    GetRouter,
    RouterId,
    NoRouterData,
}

#[derive(Clone)]
pub struct NodeInfo {
    pub ip: String,
    pub port: String,
}

impl FromStr for NodeInfo {
    type Err = &'static str;

    fn from_str(input: &str) -> Result<NodeInfo, &'static str> {
        let mut parts = input.split_whitespace();

        let ip = match parts.next() {
            Some(ip) => ip.to_string(),
            None => return Err("Missing ip"),
        };

        let port = match parts.next() {
            Some(port) => port.to_string(),
            None => return Err("Missing port"),
        };

        Ok(NodeInfo { ip, port })
    }
}

#[derive(Clone)]
/// Message struct is used to send commands between clients and server
pub struct Message {
    pub message_type: MessageType,
    pub payload: Option<f64>,
    pub node_info: Option<NodeInfo>,
}

/// Implementing Display for Message
impl fmt::Debug for Message {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        f.debug_struct("Message")
            .field("message_type", &self.message_type)
            .field("payload", &self.payload)
            .finish()
    }
}

impl Message {
    /// Create a new Message
    pub fn new(
        message_type: MessageType,
        payload: Option<f64>,
        node_info: Option<NodeInfo>,
    ) -> Self {
        Message {
            message_type,
            payload,
            node_info,
        }
    }

    // Serialize the Message to a String
    pub fn to_string(&self) -> String {
        let mut result = String::new();

        result.push_str(match self.message_type {
            MessageType::InitConnection => "INIT_CONNECTION",
            MessageType::AskMemoryUpdate => "ASK_MEMORY_UPDATE",
            MessageType::MemoryUpdate => "MEMORY_UPDATE",
            MessageType::AskInsertionAcceptance => "ASK_INSERTION_ACCEPTANCE",
            MessageType::Agreed => "AGREED",
            MessageType::Denied => "DENIED",
            MessageType::GetRouter => "GET_ROUTER",
            MessageType::RouterId => "ROUTER_ID",
            MessageType::NoRouterData => "NO_ROUTER_DATA",
        });

        result.push(' ');
        if let Some(payload) = self.payload {
            result.push_str(&payload.to_string());
        } else {
            result.push_str("None");
        }

        result + "\n"
    }

    // Deserialize the Message from a String
    pub fn from_string(input: &str) -> Result<Message, &'static str> {
        let mut parts = input.split_whitespace();

        let message_type = match parts.next() {
            Some("INIT_CONNECTION") => MessageType::InitConnection,
            Some("ASK_MEMORY_UPDATE") => MessageType::AskMemoryUpdate,
            Some("MEMORY_UPDATE") => MessageType::MemoryUpdate,
            Some("ASK_INSERTION_ACCEPTANCE") => MessageType::AskInsertionAcceptance,
            Some("AGREED") => MessageType::Agreed,
            Some("DENIED") => MessageType::Denied,
            Some("GET_ROUTER") => MessageType::GetRouter,
            Some("ROUTER_ID") => MessageType::RouterId,
            Some("NO_ROUTER_DATA") => MessageType::NoRouterData,
            _ => return Err("Invalid message type"),
        };

        let payload = match parts.next() {
            Some("None") => None,
            Some(payload) => Some(payload.parse().unwrap()),
            None => return Err("Missing payload"),
        };

        let node_info = match parts.next() {
            Some("None") => None,
            Some(node_info) => Some(node_info.parse().unwrap()),
            None => return Err("Missing node_info"),
        };

        Ok(Message::new(message_type, payload, node_info))
    }
}

impl PartialEq for Message {
    fn eq(&self, other: &Message) -> bool {
        self.message_type == other.message_type && self.payload == other.payload
    }
}

#[cfg(test)]

mod tests {
    use super::*;

    #[test]
    fn test_message_to_string() {
        let message = Message::new(MessageType::InitConnection, Some(0.5), None);
        assert_eq!(message.to_string(), "INIT_CONNECTION 0.5\n");
    }

    #[test]
    fn test_message_from_string() {
        let message = Message::new(MessageType::InitConnection, Some(0.5), None);
        let message_string = message.to_string();
        assert_eq!(Message::from_string(&message_string).unwrap(), message);
    }
}
