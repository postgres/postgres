use crate::node::messages::node_info::NodeInfo;

/// Enum to represent the data returned by `get_data`
#[derive(Debug, Clone)]
pub struct MessageData {
    pub payload: Option<f64>,
    pub node_info: Option<NodeInfo>,
    pub query: Option<String>,
}

impl MessageData {
    // - Constructors -

    pub fn new_payload(payload: f64) -> Self {
        MessageData {
            payload: Some(payload),
            node_info: None,
            query: None,
        }
    }

    pub fn new_node_info(node_info: NodeInfo) -> Self {
        MessageData {
            payload: None,
            node_info: Some(node_info),
            query: None,
        }
    }

    pub fn new_query(query: String) -> Self {
        MessageData {
            payload: None,
            node_info: None,
            query: Some(query),
        }
    }

    pub fn new_none() -> Self {
        MessageData {
            payload: None,
            node_info: None,
            query: None,
        }
    }

    /// Returns the attributes as a string
    pub fn to_string(&self) -> String {
        let mut attributes_to_string = String::new();

        if let Some(payload) = self.payload {
            attributes_to_string.push_str(&payload.to_string());
        }

        if let Some(node_info) = self.node_info.clone() {
            attributes_to_string.push_str(&node_info.to_string());
        }

        if let Some(query) = self.query.clone() {
            attributes_to_string.push_str(&query);
        }

        attributes_to_string
    }
}

impl PartialEq for MessageData {
    fn eq(&self, other: &Self) -> bool {
        self.payload == other.payload
            && self.node_info == other.node_info
            && self.query == other.query
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_message_data_payload() {
        let message_data = MessageData::new_payload(1.0);
        assert_eq!(message_data.to_string(), "1");
    }

    #[test]
    fn test_message_data_node_info() {
        let node_info = NodeInfo {
            ip: "1".to_string(),
            port: "2".to_string(),
        };

        let message_data = MessageData::new_node_info(node_info.clone());
        assert_eq!(message_data.to_string(), node_info.to_string());
    }

    #[test]
    fn test_message_data_query() {
        let message_data = MessageData::new_query("SELECT * FROM table".to_string());
        assert_eq!(message_data.to_string(), "SELECT * FROM table");
    }

    #[test]
    fn test_message_data_none() {
        let message_data = MessageData::new_none();
        assert_eq!(message_data.to_string(), "");
    }
}
