use inline_colorization::*;
use libc::statvfs;
use std::ffi::CString;
use std::io;
use sysinfo::System;

/// This struct represents the Memory Manager in the distributed system.
/// It will manage the memory of the node and will be used to determine if the node should accept new requests.
/// It holds the unavailable_memory_perc, which is the percentage of memory that should not be used and is reserved for the system to use for other purposes.
pub struct MemoryManager {
    unavailable_memory_perc: f64,
    pub available_memory_perc: f64,
}

impl MemoryManager {
    pub fn new(unavailable_memory_perc: f64) -> Self {
        let available_memory_perc = match Self::get_available_memory_percentage(unavailable_memory_perc) {
            Some(perc) => perc,
            None => panic!("[MemoryManager] Failed to get available memory"),
        };
        println!(
            "{color_blue}[Memory Manager] Memory Threshold: {:?}%{style_reset}",
            unavailable_memory_perc
        );
        MemoryManager {
            unavailable_memory_perc,
            available_memory_perc,
        }
    }

    pub fn update(&mut self) -> Result<(), io::Error> {
        self.available_memory_perc = match Self::get_available_memory_percentage(self.unavailable_memory_perc) {
            Some(perc) => perc,
            None => {
                return Err(io::Error::new(
                    io::ErrorKind::Other,
                    "[MemoryManager] Failed to get available memory",
                ))
            }
        };
        Ok(())
    }

    fn get_available_memory_percentage(unavailable_memory_perc: f64) -> Option<f64> {
        if unavailable_memory_perc == 100.0 {
            return Some(0.0);
        }

        // Create a System object
        let mut sys = System::new_all();

        // Refresh system data
        sys.refresh_all();

        // Get the root directory information
        let path = CString::new("/").unwrap();
        let mut stat: statvfs = unsafe { std::mem::zeroed() };

        if unsafe { statvfs(path.as_ptr(), &mut stat) } == 0 {
            let total_space = ((stat.f_blocks as u64) * stat.f_frsize) / 1024;
            let available_space = ((stat.f_bavail as u64) * stat.f_frsize) / 1024;

            // if percentage is greater than 1, it means that the total of space used exceeds the threshold.
            // If so, return 0
            let total = total_space as f64;
            let threshold_size = total * (unavailable_memory_perc / 100.0);

            if threshold_size > available_space as f64 {
                println!(
                    "{color_red}[Memory Manager] Memory Threshold Exceeded Available Space{style_reset}"
                );
                return Some(0.0);
            }
            
            let usable_available_space = available_space as f64 - threshold_size;
            let usable_total_space = total - threshold_size / 100.0;
            
            let percentage = usable_available_space / usable_total_space * 100.0;
            if percentage > 100.0 {
                return Some(0.0);
            }
            println!(
                "{color_blue}[Memory Manager] Available Memory: {:?} %{style_reset}",
                percentage
            );
            Some(percentage)
        } else {
            None
        }
    }
}


#[cfg(test)]

mod tests {
    use super::*;

    #[test]
    fn test_get_available_memory_percentage() {
        let unavailable_memory_perc = 0.0;
        let available_memory_perc = MemoryManager::get_available_memory_percentage(unavailable_memory_perc);
        assert_eq!(available_memory_perc.is_some(), true);
    }

    #[test]
    fn test_get_available_memory_percentage_threashold_is_total() {
        let unavailable_memory_perc = 100.0;
        let available_memory_perc = MemoryManager::get_available_memory_percentage(unavailable_memory_perc).unwrap();
        assert_eq!(available_memory_perc, 0.0);
    }

    #[test]
    fn test_get_available_memory_percentage_threashold_exceeds_available_space() {
        let unavailable_memory_perc = 90.0;
        let available_memory_perc = MemoryManager::get_available_memory_percentage(unavailable_memory_perc).unwrap();
        assert_eq!(available_memory_perc, 0.0);
    }
}