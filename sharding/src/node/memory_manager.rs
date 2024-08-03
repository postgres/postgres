use std::io;
use futures::future::ErrInto;
use sysinfo::System;
use std::ffi::CString;
use libc::statvfs;
use inline_colorization::*;

pub struct MemoryManager {
    memory_threshold: f64,
    pub available_memory_perc: f64,
}

impl MemoryManager {
    pub fn new(memory_threshold: f64) -> Self {
        let available_memory_perc = match Self::get_available_memory_percentage() {
            Some(perc) => perc,
            None => panic!("[MemoryManager] Failed to get available memory"),
        };
        println!("{color_blue}[Memory Manager] Memory Threshold: {:?}{style_reset}", memory_threshold);
        MemoryManager {
            memory_threshold,
            available_memory_perc,
        }
    }

    pub fn accepts_insertions(&self) -> bool {
        self.available_memory_perc > self.memory_threshold
    }

    pub fn update(&mut self) -> Result<(), io::Error> {
        self.available_memory_perc = match Self::get_available_memory_percentage() {
            Some(perc) => perc,
            None => return Err(io::Error::new(io::ErrorKind::Other, "[MemoryManager] Failed to get available memory")),
        };
        Ok(())
    }

    fn get_available_memory_percentage() -> Option<f64> {
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
    
            Some((available_space as f64 / total_space as f64) * 100.0)
        } else {
            None
        }
    }
}