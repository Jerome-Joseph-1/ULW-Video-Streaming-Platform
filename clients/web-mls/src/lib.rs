//! OpenMLS (RFC 9420) for the browser, compiled to WebAssembly: the same library, version,
//! ciphersuite and message forms as the FFI bridge the C++ harnesses use (ADR-0044, ADR-0099),
//! so a browser and a native client share groups through the chat server, which carries each
//! MLSMessage as an opaque body.

pub mod core;

#[cfg(target_arch = "wasm32")]
mod wasm;
