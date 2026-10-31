#include <fstream>
#include <map>
#include <string>

// Parser of the BlueZ bond file /var/lib/bluetooth/<adapter>/<device>/info.
// Only the keys BTstack needs are read: IRK and the LTK with its parameters.

#include "ble/bond_parser.h"

// Minimal INI reader: "[Section]" + "Key=Value" -> map["Section.Key"]
static std::map<std::string, std::string> read_ini(const char* path) {
  std::map<std::string, std::string> kv;
  std::ifstream f(path);
  std::string line, section;
  while (std::getline(f, line)) {
    if (line.empty() || line[0] == '#')
      continue;
    if (line.front() == '[' && line.back() == ']') {
      section = line.substr(1, line.size() - 2);
      continue;
    }
    auto eq = line.find('=');
    if (eq != std::string::npos)
      kv[section + "." + line.substr(0, eq)] = line.substr(eq + 1);
  }
  return kv;
}

// 32 hex digits -> 16-byte key. BlueZ writes keys little-endian, BTstack
// sm_key_t is MSB-first, so the bytes are reversed.
static bool parse_key(const std::string& hex, sm_key_t out) {
  if (hex.size() != 32)
    return false;
  sm_key_t le;
  for (int i = 0; i < 16; i++) {
    int hi = nibble_for_char(hex[2 * i]), lo = nibble_for_char(hex[2 * i + 1]);
    if (hi < 0 || lo < 0)
      return false;
    le[i] = (uint8_t)((hi << 4) | lo);
  }
  reverse_128(le, out);
  return true;
}

// The LTK section name depends on the BlueZ version and the role;
// Peripheral* and Slave* are the same key under the new and the old name
static std::string find_ltk_section(std::map<std::string, std::string>& kv) {
  for (const char* s : {"LongTermKey", "PeripheralLongTermKey", "SlaveLongTermKey"})
    if (kv.count(std::string(s) + ".Key"))
      return s;
  return {};
}

// Returns false if a section or key is missing; std::stoi & co. throw on
// malformed numbers, so the caller wraps it in try/catch
bool load_bond(const char* path, Bond& bond) {
  auto kv = read_ini(path);
  std::string ltk = find_ltk_section(kv);
  if (ltk.empty())
    return false;
  if (!parse_key(kv["IdentityResolvingKey.Key"], bond.irk))
    return false;
  if (!parse_key(kv[ltk + ".Key"], bond.ltk))
    return false;

  // BlueZ: 0/1 = legacy (unauth/auth), 2/3 = Secure Connections (unauth/auth)
  int type = std::stoi(kv[ltk + ".Authenticated"]);
  bond.authenticated = (type == 1 || type == 3);
  bond.secure_connection = (type >= 2);
  bond.key_size = std::stoi(kv[ltk + ".EncSize"]);
  bond.ediv = (uint16_t)std::stoul(kv[ltk + ".EDiv"]);

  // EDiv/Rand identify a legacy LTK; for Secure Connections both are 0.
  // BTstack keeps Rand big-endian: high 32 bits at offset 0 (see sm.c).
  uint64_t rand = std::stoull(kv[ltk + ".Rand"]);
  big_endian_store_32(bond.rand, 0, (uint32_t)(rand >> 32));
  big_endian_store_32(bond.rand, 4, (uint32_t)rand);

  // "public" or "static" (random static identity); missing in old files: public
  bond.identity_type =
      kv["General.AddressType"] == "static" ? BD_ADDR_TYPE_LE_RANDOM : BD_ADDR_TYPE_LE_PUBLIC;
  return true;
}

// Address in the directory `up` levels above the file: 1 = the peer, 2 = the adapter
static bool addr_from_path(const char* path, int up, bd_addr_t addr) {
  std::string p(path);
  for (int i = 0; i < up; i++) {
    auto slash = p.rfind('/');
    if (slash == std::string::npos)
      return false;
    p.resize(slash);
  }
  auto start = p.rfind('/');
  std::string dir = p.substr(start == std::string::npos ? 0 : start + 1);
  // "AA:BB:CC:DD:EE:FF": sscanf_bd_addr also accepts other forms, so check the length
  return dir.size() == 17 && sscanf_bd_addr(dir.c_str(), addr) == 1;
}

bool identity_from_path(const char* path, bd_addr_t addr) {
  return addr_from_path(path, 1, addr);
}

bool adapter_from_path(const char* path, bd_addr_t addr) {
  return addr_from_path(path, 2, addr);
}
