#ifndef COUNTING_BLOOM_FILTER_BLOOM_FILTER_H_
#define COUNTING_BLOOM_FILTER_BLOOM_FILTER_H_

#include <algorithm>
#include <assert.h>

#include "hashutil.h"

#if defined(__BMI2__)
#include <immintrin.h>
#endif

using namespace std;
using namespace hashing;

namespace counting_bloomfilter {

inline int bitCount64(uint64_t x) {
    return __builtin_popcountll(x);
}

inline int select64(uint64_t x, int n) {
#if defined(__BMI2__)
    // with this, "add" is around 310 ns/key at 10000000 keys
    // from http://bitmagic.io/rank-select.html
    // https://github.com/Forceflow/libmorton/issues/6
    // This is a rather unusual usage of the pdep (bit deposit) operation,
    // as we use the x as the mask, and we use n as the value.
    // We deposit (move) the bits of 1 << n to the locations
    // defined by x.
    uint64_t d = _pdep_u64(1ULL << n, x);
    // and now we count the trailing zeroes, to find out
    // where the '1' was deposited
    return __builtin_ctzl(d);
    // return _tzcnt_u64(d);
#else
    // alternative implementation
    // with this, "add" is around 680 ns/key at 10000000 keys
    for(int i = 0; i < 64; i++) {
        if ((x & 1) == 1) {
            if (n-- == 0) {
                return i;
            }
        }
        x >>= 1;
    }
    return -1;
#endif
}

inline int numberOfLeadingZeros64(uint64_t x) {
    // If x is 0, the result is undefined.
    return __builtin_clzl(x);
}

enum Status {
  Ok = 0,
  NotFound = 1,
  NotEnoughSpace = 2,
  NotSupported = 3,
};

inline uint32_t reduce(uint32_t hash, uint32_t n) {
  // http://lemire.me/blog/2016/06/27/a-fast-alternative-to-the-modulo-reduction/
  return (uint32_t)(((uint64_t)hash * n) >> 32);
}

// CountingBloomFilter --------------------------------------------------------------------------------------

template <typename ItemType, size_t bits_per_item, bool branchless,
          typename HashFamily = TwoIndependentMultiplyShift,
          int k = (int)((double)bits_per_item * 0.693147180559945 + 0.5)>
class CountingBloomFilter {

  uint64_t *data;
  size_t arrayLength;
  HashFamily hasher;
  const int blockShift = 14;
  const int blockLen = 1 << blockShift;

  void AddBlock(uint32_t *tmp, int block, int len);

public:
  explicit CountingBloomFilter(const size_t n) : hasher() {
    size_t bitCount = 4 * n * bits_per_item;
    this->arrayLength = (bitCount + 63) / 64;
    data = new uint64_t[arrayLength]();
  }
  ~CountingBloomFilter() { delete[] data; }
  Status Add(const ItemType &item);
  Status AddAll(const vector<ItemType> data, const size_t start, const size_t end);
  Status Remove(const ItemType &item);
  Status Contain(const ItemType &item) const;
  size_t SizeInBytes() const { return arrayLength * 8; }
};

template <typename ItemType, size_t bits_per_item, bool branchless,
          typename HashFamily, int k>
Status CountingBloomFilter<ItemType, bits_per_item, branchless, HashFamily, k>::
    Add(const ItemType &key) {
  uint64_t hash = hasher(key);
  uint32_t a = (uint32_t)(hash >> 32);
  uint32_t b = (uint32_t)hash;
  for (int i = 0; i < k; i++) {
    uint index = reduce(a, this->arrayLength);
    data[index] += 1ULL << ((a << 2) & 0x3f);
    a += b;
  }
  return Ok;
}

template <typename ItemType, size_t bits_per_item, bool branchless,
          typename HashFamily, int k>
void CountingBloomFilter<ItemType, bits_per_item, branchless, HashFamily, k>::
    AddBlock(uint32_t *tmp, int block, int len) {
  for (int i = 0; i < len; i++) {
    int index = tmp[(block << blockShift) + i];
    data[index >> 4] += 1ULL << ((index << 2) & 0x3f);
  }
}

template <typename ItemType, size_t bits_per_item, bool branchless,
          typename HashFamily, int k>
Status CountingBloomFilter<ItemType, bits_per_item, branchless, HashFamily, k>::
    AddAll(const vector<ItemType> keys, const size_t start, const size_t end) {
  int blocks = 1 + arrayLength / blockLen;
  uint32_t *tmp = new uint32_t[blocks * blockLen];
  int *tmpLen = new int[blocks]();
  for (size_t i = start; i < end; i++) {
    uint64_t key = keys[i];
    uint64_t hash = hasher(key);
    uint32_t a = (uint32_t)(hash >> 32);
    uint32_t b = (uint32_t)hash;
    for (int j = 0; j < k; j++) {
      int index = reduce(a, this->arrayLength);
      int block = index >> blockShift;
      int len = tmpLen[block];
      tmp[(block << blockShift) + len] = (index << 4) + (a & 0xf);
      tmpLen[block] = len + 1;
      if (len + 1 == blockLen) {
        AddBlock(tmp, block, len + 1);
        tmpLen[block] = 0;
      }
      a += b;
    }
  }
  for (int block = 0; block < blocks; block++) {
    AddBlock(tmp, block, tmpLen[block]);
  }
  delete[] tmp;
  delete[] tmpLen;
  return Ok;
}

template <typename ItemType, size_t bits_per_item, bool branchless,
          typename HashFamily, int k>
Status CountingBloomFilter<ItemType, bits_per_item, branchless, HashFamily, k>::
    Remove(const ItemType &key) {
  uint64_t hash = hasher(key);
  uint32_t a = (uint32_t)(hash >> 32);
  uint32_t b = (uint32_t)hash;
  for (int i = 0; i < k; i++) {
    uint index = reduce(a, this->arrayLength);
    data[index] -= 1ULL << ((a << 2) & 0x3f);
    a += b;
  }
  return Ok;
}


template <typename ItemType, size_t bits_per_item, bool branchless,
          typename HashFamily, int k>
Status CountingBloomFilter<ItemType, bits_per_item, branchless, HashFamily, k>::
    Contain(const ItemType &key) const {
  uint64_t hash = hasher(key);
  uint32_t a = (uint32_t)(hash >> 32);
  uint32_t b = (uint32_t)hash;
  for (int i = 0; i < k; i++) {
    uint index = reduce(a, this->arrayLength);
    if (((data[index] >> ((a << 2) & 0x3f)) & 0xf) == 0) {
      return NotFound;
    }
    a += b;
  }
  return Ok;
}

// SuccinctCountingBloomFilter --------------------------------------------------------------------------------------

// #define VERIFY_COUNT

template <typename ItemType, size_t bits_per_item, bool branchless,
          typename HashFamily = TwoIndependentMultiplyShift,
          int k = (int)((double)bits_per_item * 0.693147180559945 + 0.5)>
class SuccinctCountingBloomFilter {

  uint64_t *data;
  uint64_t *counts;
  uint64_t *overflow;
#ifdef VERIFY_COUNT
  uint8_t *realCount;
#endif
  size_t arrayLength;
  size_t overflowLength;
  size_t nextFreeOverflow;
  HashFamily hasher;
  const int blockShift = 14;
  const int blockLen = 1 << blockShift;

  void Increment(size_t group, int bit);
  void Decrement(size_t group, int bit);
  int ReadCount(size_t group, int bit);
  void AddBlock(uint32_t *tmp, int block, int len);

public:
  explicit SuccinctCountingBloomFilter(const size_t n) : hasher() {
    size_t bitCount = n * bits_per_item;
    this->arrayLength = (bitCount + 63) / 64;
    this->overflowLength = 100 + arrayLength / 100 * 12;
    data = new uint64_t[arrayLength]();
    counts = new uint64_t[arrayLength]();
    overflow = new uint64_t[overflowLength]();
#ifdef VERIFY_COUNT
    realCount = new uint8_t[arrayLength * 64]();
#endif
    nextFreeOverflow = 0;
    for (size_t i = 0; i < overflowLength; i += 4) {
        overflow[i] = i + 4;
    }
  }
  ~SuccinctCountingBloomFilter() { delete[] data; delete[] counts; delete[] overflow; }
  Status Add(const ItemType &item);
  Status AddAll(const vector<ItemType> data, const size_t start, const size_t end);
  Status Remove(const ItemType &item);
  Status Contain(const ItemType &item) const;
  size_t SizeInBytes() const { return arrayLength * 8 * 2 + overflowLength * 8; }
};

template <typename ItemType, size_t bits_per_item, bool branchless,
          typename HashFamily, int k>
Status SuccinctCountingBloomFilter<ItemType, bits_per_item, branchless, HashFamily, k>::
    Add(const ItemType &key) {
  uint64_t hash = hasher(key);
  uint32_t a = (uint32_t)(hash >> 32);
  uint32_t b = (uint32_t)hash;
  for (int i = 0; i < k; i++) {
    uint group = reduce(a, this->arrayLength);
    Increment(group, a & 63);
    a += b;
  }
  return Ok;
}

template <typename ItemType, size_t bits_per_item, bool branchless,
          typename HashFamily, int k>
void SuccinctCountingBloomFilter<ItemType, bits_per_item, branchless, HashFamily, k>::
    AddBlock(uint32_t *tmp, int block, int len) {
  for (int i = 0; i < len; i++) {
    uint32_t index = tmp[(block << blockShift) + i];
    uint32_t group = index >> 6;
    Increment(group, index & 63);
  }
}

template <typename ItemType, size_t bits_per_item, bool branchless,
          typename HashFamily, int k>
Status SuccinctCountingBloomFilter<ItemType, bits_per_item, branchless, HashFamily, k>::
    Remove(const ItemType &key) {
  uint64_t hash = hasher(key);
  uint32_t a = (uint32_t)(hash >> 32);
  uint32_t b = (uint32_t)hash;
  for (int i = 0; i < k; i++) {
    uint group = reduce(a, this->arrayLength);
    Decrement(group, a & 63);
    a += b;
  }
  return Ok;
}

template <typename ItemType, size_t bits_per_item, bool branchless,
          typename HashFamily, int k>
Status SuccinctCountingBloomFilter<ItemType, bits_per_item, branchless, HashFamily, k>::
    AddAll(const vector<ItemType> keys, const size_t start, const size_t end) {
  int blocks = 1 + arrayLength / blockLen;
  uint32_t *tmp = new uint32_t[blocks * blockLen];
  int *tmpLen = new int[blocks]();
  for (size_t i = start; i < end; i++) {
    uint64_t key = keys[i];
    uint64_t hash = hasher(key);
    uint32_t a = (uint32_t)(hash >> 32);
    uint32_t b = (uint32_t)hash;
    for (int j = 0; j < k; j++) {
      int index = reduce(a, this->arrayLength);
      int block = index >> blockShift;
      int len = tmpLen[block];
      tmp[(block << blockShift) + len] = (index << 6) + (a & 63);
      tmpLen[block] = len + 1;
      if (len + 1 == blockLen) {
        AddBlock(tmp, block, len + 1);
        tmpLen[block] = 0;
      }
      a += b;
    }
  }
  for (int block = 0; block < blocks; block++) {
    AddBlock(tmp, block, tmpLen[block]);
  }
  delete[] tmp;
  delete[] tmpLen;
  return Ok;
}

template <typename ItemType, size_t bits_per_item, bool branchless,
          typename HashFamily, int k>
void SuccinctCountingBloomFilter<ItemType, bits_per_item, branchless, HashFamily, k>::
    Increment(size_t group, int bit) {
#ifdef VERIFY_COUNT
    realCount[(group << 6) + bit]++;
#endif
    uint64_t m = data[group];
    uint64_t c = counts[group];
    if ((c & 0xc000000000000000ULL) != 0) {
        // an overflow entry, or overflowing now
        size_t index;
        if ((c & 0x8000000000000000ULL) == 0) {
            // convert to an overflow entry
            // allocate overflow
            index = nextFreeOverflow;
            if (index >= overflowLength) {
                ::std::cout << "ERROR: overflow too small\n";
                data[group] |= 1ULL << bit;
                return;
            }
            nextFreeOverflow = (size_t) overflow[index];
            overflow[index] = 0;
            overflow[index + 1] = 0;
            overflow[index + 2] = 0;
            overflow[index + 3] = 0;
            // convert to a pointer
            for (int i = 0; i < 64; i++) {
                int n = ReadCount(group, i);
                overflow[index + i / 16] += n * (1ULL << (i * 4));
            }
            uint64_t count = 64;
            c = 0x8000000000000000ULL | (count << 32) | index;
            counts[group] = c;
        } else {
            // already
            index = (size_t) (c & 0x0fffffffULL);
            c += 1ULL << 32;
            counts[group] = c;
        }
        overflow[index + bit / 16] += (1ULL << (bit * 4));
        data[group] |= 1ULL << bit;
    } else {
        data[group] |= 1ULL << bit;
        int bitsBefore = bitCount64(m & (0xffffffffffffffffULL >> (63 - bit)));
        int before = select64((c << 1) | 1, bitsBefore);
        int d = (m >> bit) & 1;
        int insertAt = before - d;
        uint64_t mask = (1ULL << insertAt) - 1;
        uint64_t left = c & ~mask;
        uint64_t right = c & mask;
        c = (left << 1) | ((1ULL ^ d) << insertAt) | right;
        counts[group] = c;
    }
#ifdef VERIFY_COUNT
    for(int b = 0; b < 64; b++) {
        if (realCount[(group << 6) + b] != ReadCount(group, b)) {
            ::std::cout << "group " << group << "/" << b << " of " << bit << "\n";
        }
    }
#endif
}

template <typename ItemType, size_t bits_per_item, bool branchless,
          typename HashFamily, int k>
int SuccinctCountingBloomFilter<ItemType, bits_per_item, branchless, HashFamily, k>::
    ReadCount(size_t group, int bit) {
    uint64_t m = data[group];
    uint64_t d = (m >> bit) & 1;
    if (d == 0) {
        return 0;
    }
    uint64_t c = counts[group];
    if ((c & 0x8000000000000000ULL) != 0) {
        size_t index = (size_t) (c & 0x0fffffffULL);
        uint64_t n = overflow[index + bit / 16];
        n >>= 4 * (bit & 0xf);
        return (int) (n & 15);
    }
    int bitsBefore = bitCount64(m & (0xffffffffffffffffULL >> (63 - bit)));
    int bitPos = select64(c, bitsBefore - 1);
    uint64_t y = ((c << (63 - bitPos)) << 1) | (1ULL << (63 - bitPos));
    return numberOfLeadingZeros64(y) + 1;
}

template <typename ItemType, size_t bits_per_item, bool branchless,
          typename HashFamily, int k>
void SuccinctCountingBloomFilter<ItemType, bits_per_item, branchless, HashFamily, k>::
    Decrement(size_t group, int bit) {
#ifdef VERIFY_COUNT
    realCount[(group << 6) + bit]--;
#endif
    uint64_t m = data[group];
    uint64_t c = counts[group];
    if ((c & 0x8000000000000000ULL) != 0) {
        // an overflow entry
        size_t index = (size_t) (c & 0x0fffffffULL);
        size_t count = (size_t) (c >> 32) & 0x0fffffffULL;
        c -= 1ULL << 32;
        counts[group] = c;
        uint64_t n = overflow[index + bit / 16];
        overflow[index + bit / 16] = n - (1ULL << (bit * 4));
        n >>= 4 * (bit & 0xf);
        if ((n & 0xf) == 1) {
            data[group] &= ~(1ULL << bit);
        }
        if (count < 64) {
            // convert back to an inline entry, and free up the overflow entry
            uint64_t c2 = 0;
            for (int j = 63; j >= 0; j--) {
                int cj = (int) ((overflow[index + j / 16] >> (4 * j)) & 0xf);
                if (cj > 0) {
                    c2 = ((c2 << 1) | 1) << (cj - 1);
                }
            }
            counts[group] = c2;
            // free overflow
            overflow[index] = nextFreeOverflow;
            nextFreeOverflow = index;
        }
    } else {
        int bitsBefore = bitCount64(m & (0xffffffffffffffffULL >> (63 - bit)));
        int before = select64((c << 1) | 1, bitsBefore) - 1;
        int removeAt = max(0, before - 1);
        // remove the bit from the counter
        uint64_t mask = (1ULL << removeAt) - 1;
        uint64_t left = (c >> 1) & ~mask;
        uint64_t right= c & mask;
        counts[group] = left | right;
        uint64_t removed = (c >> removeAt) & 1;
        // possibly reset the data bit
        data[group] = m & ~(removed << bit);
    }
#ifdef VERIFY_COUNT
    for(int b = 0; b < 64; b++) {
        if (realCount[(group << 6) + b] != ReadCount(group, b)) {
            ::std::cout << "group- " << group << "/" << b << " of " << bit << "\n";
        }
    }
#endif
}

template <typename ItemType, size_t bits_per_item, bool branchless,
          typename HashFamily, int k>
Status SuccinctCountingBloomFilter<ItemType, bits_per_item, branchless, HashFamily, k>::
    Contain(const ItemType &key) const {
  uint64_t hash = hasher(key);
  uint32_t a = (uint32_t)(hash >> 32);
  uint32_t b = (uint32_t)hash;
  for (int i = 0; i < k; i++) {
    uint group = reduce(a, this->arrayLength);
    if (((data[group] >> (a & 63)) & 1) == 0) {
      return NotFound;
    }
    a += b;
  }
  return Ok;
}

// SuccinctCountingBlockedBloomFilter --------------------------------------------------------------------------------------


// #define VERIFY_COUNT

template <typename ItemType, size_t bits_per_item, typename HashFamily,
          int k = (int)((double)bits_per_item * 0.693147180559945 + 0.5)>
class SuccinctCountingBlockedBloomFilter {
private:
  const int bucketCount;
  HashFamily hasher;
  uint64_t *data;
  uint64_t *counts;
  uint64_t *overflow;
  size_t overflowLength;
  size_t nextFreeOverflow;
#ifdef VERIFY_COUNT
  uint8_t *realCount;
#endif

  void Increment(size_t group, int bit);
  void Decrement(size_t group, int bit);
  int ReadCount(size_t group, int bit);

public:
  explicit SuccinctCountingBlockedBloomFilter(const int capacity);
  ~SuccinctCountingBlockedBloomFilter() noexcept;
  void Add(const uint64_t key) noexcept;
  void Remove(const uint64_t key) noexcept;
  bool Contain(const uint64_t key) const noexcept;
  uint64_t SizeInBytes() const {
      return 2 * 64 * bucketCount + 8 * overflowLength;
  }
};

template <typename ItemType, size_t bits_per_item, typename HashFamily, int k>
SuccinctCountingBlockedBloomFilter<ItemType, bits_per_item, HashFamily, k>::
    SuccinctCountingBlockedBloomFilter(const int capacity)
    : bucketCount(capacity * bits_per_item / 512), hasher() {
  const size_t alloc_size = bucketCount * (512 / 8);
  const int malloc_failed =
      posix_memalign(reinterpret_cast<void **>(&data), 64, alloc_size);
  if (malloc_failed)
    throw ::std::bad_alloc();
  memset(data, 0, alloc_size);
  size_t arrayLength = bucketCount * 8;
  overflowLength = 100 + arrayLength / 100 * 36;
  counts = new uint64_t[arrayLength]();
  overflow = new uint64_t[overflowLength]();
#ifdef VERIFY_COUNT
  realCount = new uint8_t[arrayLength * 64]();
#endif
  nextFreeOverflow = 0;
  for (size_t i = 0; i < overflowLength; i += 8) {
      overflow[i] = i + 8;
  }
}

template <typename ItemType, size_t bits_per_item, typename HashFamily, int k>
SuccinctCountingBlockedBloomFilter<ItemType, bits_per_item, HashFamily, k>::
    ~SuccinctCountingBlockedBloomFilter() noexcept {
  free(data);
  delete[] counts;
  delete[] overflow;
}

static inline uint64_t rotl64(uint64_t n, unsigned int c) {
  // assumes width is a power of 2
  const unsigned int mask = (CHAR_BIT * sizeof(n) - 1);
  // assert ( (c<=mask) &&"rotate by type width or more");
  c &= mask;
  return (n << c) | (n >> ((-c) & mask));
}

template <typename ItemType, size_t bits_per_item, typename HashFamily, int k>
void SuccinctCountingBlockedBloomFilter<ItemType, bits_per_item, HashFamily, k>::
    Add(const uint64_t key) noexcept {
  const auto hash = hasher(key);
  const uint32_t bucket_start = reduce(rotl64(hash, 32), bucketCount) * 8;
  uint32_t a = (uint32_t)hash;
  if (k >= 3) {
    Increment(bucket_start + ((a >> 0) & 7), (a >> 3) & 0x3f);
    Increment(bucket_start + ((a >> 9) & 7), (a >> 12) & 0x3f);
    Increment(bucket_start + ((a >> 18) & 7), (a >> 21) & 0x3f);
//    data[bucket_start + ((a >> 0) & 7)] |= 1ULL << ((a >> 3) & 0x3f);
//    data[bucket_start + ((a >> 9) & 7)] |= 1ULL << ((a >> 12) & 0x3f);
//    data[bucket_start + ((a >> 18) & 7)] |= 1ULL << ((a >> 21) & 0x3f);
  }
  uint32_t b = (uint32_t)(hash >> 32);
  for (int i = 3; i < k; i++) {
    a += b;
    Increment(bucket_start + (a & 7), (a >> 3) & 0x3f);
//    data[bucket_start + (a & 7)] |= 1ULL << ((a >> 3) & 0x3f);
  }
}

template <typename ItemType, size_t bits_per_item, typename HashFamily, int k>
void SuccinctCountingBlockedBloomFilter<ItemType, bits_per_item, HashFamily, k>::
    Increment(size_t group, int bit) {
#ifdef VERIFY_COUNT
    realCount[(group << 6) + bit]++;
#endif
    uint64_t m = data[group];
    uint64_t c = counts[group];
    if ((c & 0xc000000000000000ULL) != 0) {
        // an overflow entry, or overflowing now
        size_t index;
        if ((c & 0x8000000000000000ULL) == 0) {
            // convert to an overflow entry
            // allocate overflow
            index = nextFreeOverflow;
            if (index >= overflowLength) {
                ::std::cout << "ERROR: overflow too small\n";
                data[group] |= 1ULL << bit;
                return;
            }
            nextFreeOverflow = (size_t) overflow[index];
            for (int i = 0; i < 8; i++) {
                overflow[index + i] = 0;
            }
            // convert to a pointer
            for (int i = 0; i < 64; i++) {
                int n = ReadCount(group, i);
                overflow[index + i / 8] += n * (1ULL << (i * 8));
            }
            uint64_t count = 64;
            c = 0x8000000000000000ULL | (count << 32) | index;
            counts[group] = c;
        } else {
            // already
            index = (size_t) (c & 0x0fffffffULL);
            c += 1ULL << 32;
            counts[group] = c;
        }
        overflow[index + bit / 8] += (1ULL << (bit * 8));
        data[group] |= 1ULL << bit;
    } else {
        data[group] |= 1ULL << bit;
        int bitsBefore = bitCount64(m & (0xffffffffffffffffULL >> (63 - bit)));
        int before = select64((c << 1) | 1, bitsBefore);
        int d = (m >> bit) & 1;
        int insertAt = before - d;
        uint64_t mask = (1ULL << insertAt) - 1;
        uint64_t left = c & ~mask;
        uint64_t right = c & mask;
        c = (left << 1) | ((1ULL ^ d) << insertAt) | right;
        counts[group] = c;
    }
#ifdef VERIFY_COUNT
    for(int b = 0; b < 64; b++) {
        if (realCount[(group << 6) + b] != ReadCount(group, b)) {
            ::std::cout << "group " << group << "/" << b << " of " << bit << "\n";
        }
    }
#endif
}

template <typename ItemType, size_t bits_per_item, typename HashFamily, int k>
int SuccinctCountingBlockedBloomFilter<ItemType, bits_per_item, HashFamily, k>::
    ReadCount(size_t group, int bit) {
    uint64_t m = data[group];
    uint64_t d = (m >> bit) & 1;
    if (d == 0) {
        return 0;
    }
    uint64_t c = counts[group];
    if ((c & 0x8000000000000000ULL) != 0) {
        size_t index = (size_t) (c & 0x0fffffffULL);
        uint64_t n = overflow[index + bit / 8];
        n >>= 8 * (bit & 0xff);
        return (int) (n & 0xff);
    }
    int bitsBefore = bitCount64(m & (0xffffffffffffffffULL >> (63 - bit)));
    int bitPos = select64(c, bitsBefore - 1);
    uint64_t y = ((c << (63 - bitPos)) << 1) | (1ULL << (63 - bitPos));
    return numberOfLeadingZeros64(y) + 1;
}

template <typename ItemType, size_t bits_per_item, typename HashFamily, int k>
void SuccinctCountingBlockedBloomFilter<ItemType, bits_per_item, HashFamily, k>::
    Remove(const uint64_t key) noexcept {
  const auto hash = hasher(key);
  const uint32_t bucket_start = reduce(rotl64(hash, 32), bucketCount) * 8;
  uint32_t a = (uint32_t)hash;
  if (k >= 3) {
    Decrement(bucket_start + ((a >> 0) & 7), (a >> 3) & 0x3f);
    Decrement(bucket_start + ((a >> 9) & 7), (a >> 12) & 0x3f);
    Decrement(bucket_start + ((a >> 18) & 7), (a >> 21) & 0x3f);
  }
  uint32_t b = (uint32_t)(hash >> 32);
  for (int i = 3; i < k; i++) {
    a += b;
    Decrement(bucket_start + (a & 7), (a >> 3) & 0x3f);
  }
}

template <typename ItemType, size_t bits_per_item, typename HashFamily, int k>
void SuccinctCountingBlockedBloomFilter<ItemType, bits_per_item, HashFamily, k>::
    Decrement(size_t group, int bit) {
#ifdef VERIFY_COUNT
    realCount[(group << 6) + bit]--;
#endif
    uint64_t m = data[group];
    uint64_t c = counts[group];
    if ((c & 0x8000000000000000ULL) != 0) {
        // an overflow entry
        size_t index = (size_t) (c & 0x0fffffffULL);
        size_t count = (size_t) (c >> 32) & 0x0fffffffULL;
        c -= 1ULL << 32;
        counts[group] = c;
        uint64_t n = overflow[index + bit / 8];
        overflow[index + bit / 8] = n - (1ULL << (bit * 8));
        n >>= 8 * (bit & 0xf);
        if ((n & 0xff) == 1) {
            data[group] &= ~(1ULL << bit);
        }
        if (count < 64) {
            // convert back to an inline entry, and free up the overflow entry
            uint64_t c2 = 0;
            for (int j = 63; j >= 0; j--) {
                int cj = (int) ((overflow[index + j / 8] >> (8 * j)) & 0xff);
                if (cj > 0) {
                    c2 = ((c2 << 1) | 1) << (cj - 1);
                }
            }
            counts[group] = c2;
            // free overflow
            overflow[index] = nextFreeOverflow;
            nextFreeOverflow = index;
        }
    } else {
        int bitsBefore = bitCount64(m & (0xffffffffffffffffULL >> (63 - bit)));
        int before = select64((c << 1) | 1, bitsBefore) - 1;
        int removeAt = max(0, before - 1);
        // remove the bit from the counter
        uint64_t mask = (1ULL << removeAt) - 1;
        uint64_t left = (c >> 1) & ~mask;
        uint64_t right= c & mask;
        counts[group] = left | right;
        uint64_t removed = (c >> removeAt) & 1;
        // possibly reset the data bit
        data[group] = m & ~(removed << bit);
    }
#ifdef VERIFY_COUNT
    for(int b = 0; b < 64; b++) {
        if (realCount[(group << 6) + b] != ReadCount(group, b)) {
            ::std::cout << "group- " << group << "/" << b << " of " << bit << "\n";
        }
    }
#endif
}

template <typename ItemType, size_t bits_per_item, typename HashFamily, int k>
bool SuccinctCountingBlockedBloomFilter<ItemType, bits_per_item, HashFamily, k>::
    Contain(const uint64_t key) const noexcept {
  const auto hash = hasher(key);
  const uint32_t bucket_start = reduce(rotl64(hash, 32), bucketCount) * 8;
  uint32_t a = (uint32_t)hash;
  char ok = 1;
  if (k >= 3) {
    ok &= data[bucket_start + ((a >> 0) & 7)] >> ((a >> 3) & 0x3f);
    ok &= data[bucket_start + ((a >> 9) & 7)] >> ((a >> 12) & 0x3f);
    ok &= data[bucket_start + ((a >> 18) & 7)] >> ((a >> 21) & 0x3f);
  }
  if (!ok) {
    return ok;
  }
  uint32_t b = (uint32_t)(hash >> 32);
  for (int i = 3; i < k; i++) {
    a += b;
    ok &= data[bucket_start + (a & 7)] >> ((a >> 3) & 63);
    if (!ok) {
      return ok;
    }
  }
  return ok;
}

}
#endif