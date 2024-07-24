use ::crypto::sha3::Sha3;
use crypto::digest::Digest;

/// Returns hash of the ip+port in which the Shard is connected
pub fn hash_shard(shard_ip: &str, shard_port: &str) -> String {
    let mut hasher = Sha3::keccak256();
    let concatenation = format!("{shard_ip}{shard_port}");
    hasher.input_str(&concatenation);
    hasher.result_str()
}

pub fn hash_data(data: Vec<String>) -> Vec<String> {
    let mut hasher = Sha3::sha3_256();
    let mut hashed_values = Vec::new();
    for value in data {
        hasher.input_str(&*value);
        hashed_values.push(hasher.result_str());
        hasher.reset();
    }
    hashed_values
}