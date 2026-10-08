use crate::identifier::{Call, Species};
use crate::locator::LocationEstimate;
use bsp_proto::Timestamp;
use serde::{Deserialize, Serialize};
use uuid::Uuid;

/// One bird vocalisation, possibly heard by several clients.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Detection {
    pub id: Uuid,
    /// Earliest call onset across clients.
    pub time: Timestamp,
    pub species: Species,
    /// Highest confidence across clients.
    pub confidence: f32,
    pub calls: Vec<Call>,
    /// The species is not expected at this site.
    #[serde(default)]
    pub unexpected: bool,
    /// Present only when enough positioned, clock-synced clients heard the call.
    pub location: Option<LocationEstimate>,
    pub created_at: Timestamp,
}
