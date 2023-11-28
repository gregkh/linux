// Copyright (c) Meta Platforms, Inc. and affiliates.

// This software may be used and distributed according to the terms of the
// GNU General Public License version 2.
extern crate bindgen;

use std::env;
use std::path::PathBuf;

use libbpf_cargo::SkeletonBuilder;

const HEADER_PATH: &str = "src/bpf/rusty.h";

fn bindgen_rusty() {
    // Tell cargo to invalidate the built crate whenever the wrapper changes
    println!("cargo:rerun-if-changed={}", HEADER_PATH);

    // The bindgen::Builder is the main entry point
    // to bindgen, and lets you build up options for
    // the resulting bindings.
    let bindings = bindgen::Builder::default()
        // The input header we would like to generate
        // bindings for.
        .header(HEADER_PATH)
        // Tell cargo to invalidate the built crate whenever any of the
        // included header files changed.
        .parse_callbacks(Box::new(bindgen::CargoCallbacks))
        // Finish the builder and generate the bindings.
        .generate()
        // Unwrap the Result and panic on failure.
        .expect("Unable to generate bindings");

    // Write the bindings to the $OUT_DIR/bindings.rs file.
    let out_path = PathBuf::from(env::var("OUT_DIR").unwrap());
    bindings
        .write_to_file(out_path.join("rusty_sys.rs"))
        .expect("Couldn't write bindings!");
}

fn gen_bpf_sched(name: &str) {
    let bpf_cflags = env::var("SCX_RUST_BPF_CFLAGS").unwrap();
    let clang = env::var("SCX_RUST_CLANG").unwrap();
    let src = format!("./src/bpf/{}.bpf.c", name);
    let out_path = PathBuf::from(env::var("OUT_DIR").unwrap());
    let skel_path = out_path.join(format!("{}.bpf.skel.rs", name));
    let obj = out_path.join(format!("{}.bpf.o", name));
    SkeletonBuilder::new()
        .source(&src)
	.obj(&obj)
        .clang(clang)
        .clang_args(bpf_cflags)
        .build_and_generate(&skel_path)
        .unwrap();
    println!("cargo:rerun-if-changed={}", src);
}

fn main() {
    bindgen_rusty();
    gen_bpf_sched("rusty");
}
