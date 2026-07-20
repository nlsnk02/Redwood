#include "cbtree/fingerprint.hpp"
namespace cbtree {
Fingerprint fingerprint(Key key) noexcept {
  // splitmix64 lower 16 bits
  uint64_t z = key + 0x9e3779b97f4a7c15ULL;
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
  z = z ^ (z >> 31);
  return static_cast<Fingerprint>(z & 0xFFFF);
}
}
