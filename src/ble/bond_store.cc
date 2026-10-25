#include "ble/bond_store.h"

#include <cstdio>
#include <cstring>

void bond_store_save(const Bond& bond) {
  int index = le_device_db_add(
      bond.identity_type, const_cast<uint8_t*>(bond.identity), const_cast<uint8_t*>(bond.irk));
  le_device_db_encryption_set(
      index, bond.ediv, const_cast<uint8_t*>(bond.rand), const_cast<uint8_t*>(bond.ltk),
      bond.key_size, bond.authenticated, /*authorized*/ 0, bond.secure_connection);
  printf("bond for %s saved, index %d\n", bd_addr_to_str(bond.identity), index);
}

// One entry: identity + IRK from le_device_db_info, LTK from _encryption_get
static bool read_entry(int index, Bond& b) {
  le_device_db_info(index, &b.identity_type, b.identity, b.irk);
  if (b.identity_type == BD_ADDR_TYPE_UNKNOWN)
    return false;  // empty slot
  int key_size = 0, authenticated = 0, authorized = 0, sc = 0;
  le_device_db_encryption_get(
      index, &b.ediv, b.rand, b.ltk, &key_size, &authenticated, &authorized, &sc);
  if (key_size == 0)
    return false;  // identity only, no LTK: unusable
  b.key_size = key_size;
  b.authenticated = authenticated;
  b.secure_connection = sc;
  return true;
}

bool bond_store_find(const uint8_t* peer, Bond& out) {
  int found = 0;
  for (int i = 0; i < le_device_db_max_count(); i++) {
    Bond b;
    if (!read_entry(i, b))
      continue;
    if (peer) {
      if (memcmp(b.identity, peer, 6) == 0) {
        out = b;
        return true;
      }
      continue;
    }
    printf("stored bond: %s\n", bd_addr_to_str(b.identity));
    if (found++ == 0)
      out = b;
  }
  if (peer) {
    printf("no bond for %s, import it with --bond\n", bd_addr_to_str(peer));
    return false;
  }
  if (found == 0)
    printf("no stored bond, import one with --bond\n");
  else if (found > 1)
    printf("%d bonds stored, pick one with --peer\n", found);
  return found == 1;
}
