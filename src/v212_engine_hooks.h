#pragma once

// Register only after the supervisor verifies the exact mage_g ELF and its
// semantic spans in retail-v2.12-engine-network.json. The settings callback is
// reached through its original guest OPD; direct generated calls are unchanged.
extern "C" void mag_v212_engine_register();
