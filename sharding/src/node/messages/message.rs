use std::fmt;

/// MessageType enum shows which command is being sent
#[derive(Debug, PartialEq, Clone)]
pub enum MessageType {
    InitConnection,
    AskMemoryUpdate,
    MemoryUpdate,
    AskInsertionAcceptance,
    Agreed,
    Denied,
}

#[derive(Clone)]
/// Message struct is used to send commands between clients and server
pub struct Message {
    pub message_type: MessageType,
    pub payload: Option<f64>,
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
    pub fn new(message_type: MessageType, payload: Option<f64>) -> Self {
        Message {
            message_type,
            payload,
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
            _ => return Err("Invalid message type"),
        };

        let payload = match parts.next() {
            Some("None") => None,
            Some(payload) => Some(payload.parse().unwrap()),
            None => return Err("Missing payload"),
        };

        Ok(Message::new(message_type, payload))
    }
}

impl PartialEq for Message {
    fn eq(&self, other: &Message) -> bool {
        self.message_type == other.message_type && self.payload == other.payload
    }
}

// TODO-SHARD implement tests