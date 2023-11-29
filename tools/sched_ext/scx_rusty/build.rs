// Copyright (c) Meta Platforms, Inc. and affiliates.

// This software may be used and distributed according to the terms of the
// GNU General Public License version 2.

fn main() {
    scx_utils::build_helpers::bindgen_bpf_intf(None, None);
    scx_utils::build_helpers::gen_bpf_skel(None, None, None);
}
