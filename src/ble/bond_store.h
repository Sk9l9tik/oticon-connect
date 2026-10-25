#ifndef BLE_BOND_STORE_H_
#define BLE_BOND_STORE_H_

#include "ble/bond_parser.h"

// Bonds in BTstack's device DB (le_device_db, persisted in the TLV file that
// main.cc opens). Usable only once the TLV is open, i.e. from HCI_STATE_WORKING.

// Adds the bond, or overwrites the entry with the same identity address
void bond_store_save(const Bond& bond);

// The bond of `peer`, or the only one in the DB when `peer` is nullptr.
// false: none, or several and no peer given (printed which).
bool bond_store_find(const uint8_t* peer, Bond& out);

#endif  // BLE_BOND_STORE_H_
