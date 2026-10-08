//! Domain logic for bsp: audio helpers, the [`identifier::Identifier`] and
//! [`locator::Locator`] abstractions, and the resulting [`detection::Detection`].

pub mod audio;
pub mod detection;
#[cfg(feature = "birdnet")]
pub mod geo;
pub mod identifier;
pub mod locator;

pub use bsp_proto::{ClientId, Timestamp};
