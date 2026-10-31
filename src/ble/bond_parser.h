#ifndef BLE_BOND_PARSER_H_
#define BLE_BOND_PARSER_H_

#include "btstack.h"

// Keys of one bonded LE device, in the form le_device_db expects
struct Bond {
  bd_addr_t identity = {};  // identity address of the peer (never on air, the RPA is)
  int identity_type = BD_ADDR_TYPE_LE_PUBLIC;  // public or static random
  sm_key_t irk;                    // Identity Resolving Key, MSB-first: resolves the RPA
  sm_key_t ltk;                    // Long Term Key, MSB-first: encrypts the link
  uint16_t ediv = 0;               // legacy LTK id, 0 for Secure Connections
  uint8_t rand[8] = {};            // legacy LTK id, 0 for Secure Connections
  int key_size = 16;               // encryption key size, bytes
  bool authenticated = false;      // paired with MITM protection
  bool secure_connection = false;  // LE Secure Connections key
};

// Reads the BlueZ file /var/lib/bluetooth/<adapter>/<device>/info (or a copy).
// The file has no address: bond.identity is left as is, see identity_from_path.
bool load_bond(const char* path, Bond& bond);

// BlueZ keeps the bond in a directory named after the peer:
// ".../00:19:8E:B9:8B:0A/info" -> 00:19:8E:B9:8B:0A. false for a copy elsewhere.
bool identity_from_path(const char* path, bd_addr_t addr);

// ".../8C:68:8B:81:C0:E7/00:19:8E:B9:8B:0A/info" -> 8C:68:8B:81:C0:E7, the
// adapter the bond belongs to. false for a copy elsewhere.
bool adapter_from_path(const char* path, bd_addr_t addr);

#endif  // BLE_BOND_PARSER_H_
