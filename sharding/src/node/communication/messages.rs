use std::fmt;

/// MessageType enum shows which command is being sent
#[derive(Debug, PartialEq, Clone)]
pub enum MessageType {
    AskInsertion,
    Agreed,
    Denied,
}

#[derive(Clone)]
/// Message struct is used to send messages between nodes
pub struct Message {
    pub message_type: MessageType,
    // More fields can be added here
}

/// Implementing Display for Message
impl fmt::Display for Message {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        write!(f, "{:?}", self.message_type)
    }
}

impl Message {
    /// Create a new Message
    pub fn new(message_type: MessageType) -> Self {
        Message { message_type }
    }

    // Serialize the Message to a String
    pub fn to_string(&self) -> String {
        let mut result = String::new();
        result.push_str(&format!("{:?}", self.message_type));
        result
    }

    // Deserialize a String into a Message
    pub fn from_string(input: &str) -> Result<Message, &'static str> {
        let message_type = match input {
            "AskInsertion" => MessageType::AskInsertion,
            "Agreed" => MessageType::Agreed,
            "Denied" => MessageType::Denied,
            _ => return Err("Invalid message type"),
        };
        Ok(Message { message_type })
    }
}

impl PartialEq for Message {
    fn eq(&self, other: &Self) -> bool {
        self.message_type == other.message_type
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_message_to_string() {
        let message = Message::new(MessageType::AskInsertion);
        assert_eq!(message.to_string(), "AskInsertion");
    }

    #[test]
    fn test_message_from_string() {
        let message = Message::from_string("AskInsertion").unwrap();
        assert_eq!(message, Message::new(MessageType::AskInsertion));
    }
} 