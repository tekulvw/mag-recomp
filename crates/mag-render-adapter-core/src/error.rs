#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Error {
    InvalidRange,
    OutsideSnapshot,
    InvalidCapacity,
    AllocationFailed,
    CapacityExceeded,
    InvalidSourceAddress,
    UnprovenProducerEpoch,
    IdentityExhausted,
}
