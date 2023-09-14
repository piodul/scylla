use std::path::PathBuf;

fn main() {
    // Force rebuild if ./gen.py is modified.
    let mut path_to_gen_py = PathBuf::new();
    path_to_gen_py.push(std::env::var("CARGO_MANIFEST_DIR").unwrap());
    path_to_gen_py.push("../gen.py");
    println!("cargo:rerun-if-changed={}", path_to_gen_py.display());
}
