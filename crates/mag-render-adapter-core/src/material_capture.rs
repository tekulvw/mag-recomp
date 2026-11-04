//! Errors shared by material source capture.
use crate::{error::Error, partition_inputs::StreamRequest};

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum MaterialCaptureError {
    InvalidAssociation,
    InvalidPass,
    Budget,
    Allocation,
    Source {
        request: StreamRequest,
        error: Error,
    },
    Extent(StreamRequest),
}
