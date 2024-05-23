#[repr(C)]
struct RustObject {
    a: i32,
    b: i32,
    message: String
}

extern "C" fn callback(target: *mut RustObject, a: i32) {
    println!("Callback triggered! with value: {}", a);
    unsafe {
        // Update the value in RustObject with the value received from the callback:
        (*target).a = a;
    }
}

#[link(name = "extlib")]
extern {
    fn register_callback(target: *mut RustObject,
                         cb: extern fn(*mut RustObject, i32)) -> i32;
    fn trigger_callback();
}

#[no_mangle]
pub extern "C" fn rust_function(number: i32) -> i32 {
    println!("Hello from Rust!");
    println!("Number: {}", number);

    unsafe {
        let mut rust_object = Box::new(RustObject { a: 5, b: 10, message: "Hello from Rust!".to_string()});

        if register_callback(&mut *rust_object, callback) == 1 {
            println!("Callback registered successfully");
        }
        println!("Triggering callback...");
        trigger_callback(); // Triggers the callback.

        // Print rust_object.a
        println!("Value of a in RustObject: {}", rust_object.a);
    }


    return 15;
}