// BooPHF library — minimal perfect hash function with fast and low memory
// construction, at the cost of (slightly) higher bits/elem than other state of
// the art libraries once built. Works with arbitrary large number of elements,
// based on a cascade of "collision-free" bit arrays.
//
// This is a slimmed-down fork: removed unused features (Progress reporter,
// disk-backed level storage, alternative HashFunctors, file_binary iterator,
// debug print, prefetch helpers). Single supported build mode: in-memory
// fastmode parallel construction via OpenMP.

#pragma once
#include <array>
#include <atomic>
#include <bit>
#include <cassert>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace boomphf {

////////////////////////////////////////////////////////////////
#pragma mark -
#pragma mark hasher
////////////////////////////////////////////////////////////////

using hash_set_t = std::array<uint64_t, 10>;
using hash_pair_t = std::array<uint64_t, 2>;

// Xorshift128*-based multi-hasher: derives N hashes per key from a single
// user-provided SingleHasher_t. Caller seeds the state via h0/h1, then
// extracts further hashes via next().
//   Reference: Sebastiano Vigna, 2014 — Xorshift128* (public domain).
template <typename Item, class SingleHasher_t>
class XorshiftHashFunctors {
  public:
    uint64_t h0(hash_pair_t& s, const Item& key) const {
        s[0] = singleHasher(key, 0xAAAAAAAA55555555ULL);
        return s[0];
    }

    uint64_t h1(hash_pair_t& s, const Item& key) const {
        s[1] = singleHasher(key, 0x33333333CCCCCCCCULL);
        return s[1];
    }

    // return next hash and update state s
    uint64_t next(hash_pair_t& s) const noexcept {
        uint64_t s1 = s[0];
        const uint64_t s0 = s[1];
        s[0] = s0;
        s1 ^= s1 << 23;
        return (s[1] = (s1 ^ s0 ^ (s1 >> 17) ^ (s0 >> 26))) + s0;
    }

    // returns all 10 hashes
    [[nodiscard]] hash_set_t operator()(const Item& key) const {
        hash_pair_t s;
        hash_set_t hset;

        hset[0] = singleHasher(key, 0xAAAAAAAA55555555ULL);
        hset[1] = singleHasher(key, 0x33333333CCCCCCCCULL);

        s[0] = hset[0];
        s[1] = hset[1];

        // constant bound — this loop is performance critical and must inline
        for (size_t ii = 2; ii < 10; ii++) {
            hset[ii] = next(s);
        }
        return hset;
    }

  private:
    SingleHasher_t singleHasher;
};

////////////////////////////////////////////////////////////////
#pragma mark -
#pragma mark BitVector
////////////////////////////////////////////////////////////////

class bitVector {

  public:
    bitVector() noexcept : _bitArray(nullptr), _size(0), _nchar(0) {}

    explicit bitVector(uint64_t n) : _size(n), _nchar(1ULL + n / 64ULL) {
        _bitArray = new uint64_t[_nchar]();
    }

    ~bitVector() { delete[] _bitArray; }

    bitVector(bitVector const& r)
        : _bitArray(nullptr), _size(r._size), _nchar(r._nchar), _ranks(r._ranks) {
        if (_nchar > 0) {
            _bitArray = new uint64_t[_nchar]();
            std::memcpy(_bitArray, r._bitArray, _nchar * sizeof(uint64_t));
        }
    }

    bitVector& operator=(bitVector const& r) {
        if (&r != this) {
            delete[] _bitArray;
            _size = r._size;
            _nchar = r._nchar;
            _ranks = r._ranks;
            if (_nchar > 0) {
                _bitArray = new uint64_t[_nchar]();
                std::memcpy(_bitArray, r._bitArray, _nchar * sizeof(uint64_t));
            } else {
                _bitArray = nullptr;
            }
        }
        return *this;
    }

    bitVector& operator=(bitVector&& r) noexcept {
        if (&r != this) {
            delete[] _bitArray;
            _size = r._size;
            _nchar = r._nchar;
            _ranks = std::move(r._ranks);
            _bitArray = r._bitArray;
            r._bitArray = nullptr;
            r._size = 0;
            r._nchar = 0;
        }
        return *this;
    }

    bitVector(bitVector&& r) noexcept : _bitArray(nullptr), _size(0), _nchar(0) {
        *this = std::move(r);
    }

    void resize(uint64_t newsize) {
        delete[] _bitArray;
        _nchar = (1ULL + newsize / 64ULL);
        _bitArray = new uint64_t[_nchar]();
        _size = newsize;
    }

    [[nodiscard]] size_t size() const noexcept { return _size; }

    [[nodiscard]] uint64_t bitSize() const noexcept {
        return (_nchar * 64ULL + _ranks.capacity() * 64ULL);
    }

    void clear() noexcept { std::memset(_bitArray, 0, _nchar * sizeof(uint64_t)); }

    // clear collisions in interval, only works with start and size multiple of 64
    void clearCollisions(uint64_t start, size_t size, bitVector* cc) {
        [[assume((start & 63) == 0)]];
        [[assume((size & 63) == 0)]];
        uint64_t ids = (start / 64ULL);
        for (uint64_t ii = 0; ii < (size / 64ULL); ii++) {
            _bitArray[ids + ii] = _bitArray[ids + ii] & (~(cc->get64(ii)));
        }
        cc->clear();
    }

    [[nodiscard]] uint64_t operator[](uint64_t pos) const noexcept {
        return (_bitArray[pos >> 6ULL] >> (pos & 63)) & 1;
    }

    // atomically return old val and set to 1.
    // memory_order_relaxed is safe here because mphf::processLevel relies on the
    // implicit barrier at the end of `#pragma omp parallel` to publish all bits
    // before clearCollisions/build_ranks read them. Do not change to a custom
    // thread pool without revisiting this ordering.
    [[nodiscard]] uint64_t atomic_test_and_set(uint64_t pos) noexcept {
        uint64_t oldval = std::atomic_ref<uint64_t>(_bitArray[pos >> 6])
                              .fetch_or(1ULL << (pos & 63), std::memory_order_relaxed);
        return (oldval >> (pos & 63)) & 1;
    }

    [[nodiscard]] uint64_t get(uint64_t pos) const noexcept { return (*this)[pos]; }

    [[nodiscard]] uint64_t get64(uint64_t cell64) const noexcept { return _bitArray[cell64]; }

    // returns final cumulative rank; offset is added to all ranks computed
    uint64_t build_ranks(uint64_t offset = 0) {
        _ranks.reserve(2 + _size / _nb_bits_per_rank_sample);

        uint64_t curent_rank = offset;
        for (size_t ii = 0; ii < _nchar; ii++) {
            if (((ii * 64) % _nb_bits_per_rank_sample) == 0) {
                _ranks.push_back(curent_rank);
            }
            curent_rank += std::popcount(_bitArray[ii]);
        }
        return curent_rank;
    }

    [[nodiscard]] uint64_t rank(uint64_t pos) const noexcept {
        uint64_t word_idx = pos / 64ULL;
        uint64_t word_offset = pos % 64;
        uint64_t block = pos / _nb_bits_per_rank_sample;
        uint64_t r = _ranks[block];
        for (uint64_t w = block * _nb_bits_per_rank_sample / 64; w < word_idx; ++w) {
            r += std::popcount(_bitArray[w]);
        }
        uint64_t mask = (uint64_t(1) << word_offset) - 1;
        r += std::popcount(_bitArray[word_idx] & mask);
        return r;
    }

    void save(std::ostream& os) const {
        os.write(reinterpret_cast<char const*>(&_size), sizeof(_size));
        os.write(reinterpret_cast<char const*>(&_nchar), sizeof(_nchar));
        os.write(reinterpret_cast<char const*>(_bitArray),
                 (std::streamsize)(sizeof(uint64_t) * _nchar));
        size_t sizer = _ranks.size();
        os.write(reinterpret_cast<char const*>(&sizer), sizeof(size_t));
        os.write(reinterpret_cast<char const*>(_ranks.data()),
                 (std::streamsize)(sizeof(_ranks[0]) * _ranks.size()));
    }

    void load(std::istream& is) {
        is.read(reinterpret_cast<char*>(&_size), sizeof(_size));
        is.read(reinterpret_cast<char*>(&_nchar), sizeof(_nchar));
        this->resize(_size);
        is.read(reinterpret_cast<char*>(_bitArray), (std::streamsize)(sizeof(uint64_t) * _nchar));

        size_t sizer;
        is.read(reinterpret_cast<char*>(&sizer), sizeof(size_t));
        _ranks.resize(sizer);
        is.read(reinterpret_cast<char*>(_ranks.data()),
                (std::streamsize)(sizeof(_ranks[0]) * _ranks.size()));
    }

  protected:
    uint64_t* _bitArray;
    uint64_t _size;
    uint64_t _nchar;

    // additional size for rank is epsilon * _size
    static constexpr uint64_t _nb_bits_per_rank_sample = 512;
    std::vector<uint64_t> _ranks;
};

////////////////////////////////////////////////////////////////
#pragma mark -
#pragma mark level
////////////////////////////////////////////////////////////////

[[nodiscard]] static inline constexpr uint64_t fastrange64(uint64_t word, uint64_t p) noexcept {
    return static_cast<uint64_t>(((__uint128_t)word * (__uint128_t)p) >> 64);
}

class level {
  public:
    level() = default;
    ~level() = default;

    [[nodiscard]] uint64_t get(uint64_t hash_raw) const noexcept {
        uint64_t hashi = fastrange64(hash_raw, hash_domain);
        return bitset.get(hashi);
    }

    uint64_t idx_begin = 0;
    uint64_t hash_domain = 0;
    bitVector bitset;
};

////////////////////////////////////////////////////////////////
#pragma mark -
#pragma mark mphf
////////////////////////////////////////////////////////////////

static constexpr std::size_t NBBUFF = 10'000;

template <typename Range> struct thread_args {
    void* boophf;
    Range const* range;
    // polymorphic iterator: shared_ptr<void> allows switching between
    // input_range and fastmode iterators at the same level
    std::shared_ptr<void> it_p;
    std::shared_ptr<void> until_p;
    int level;
};

// forward declaration
template <typename elem_t, typename Hasher_t, typename Range, typename it_type>
void thread_processLevel(thread_args<Range>* args);

/* Hasher_t returns a single hash when operator()(elem_t key) is called.
   Used with XorshiftHashFunctors, it must support:
       operator()(elem_t key, uint64_t seed) */
template <typename elem_t, typename Hasher_t> class mphf {

    using MultiHasher_t = XorshiftHashFunctors<elem_t, Hasher_t>;

  public:
    mphf() = default;
    ~mphf() = default;

    // perc_elem_loaded: percent of elements kept in RAM for fastmode
    // construction (default 3%); set to 0 to disable.
    template <typename Range>
    mphf(size_t n, Range const& input_range, int num_thread = 1, double gamma = 2.0,
         float perc_elem_loaded = 0.03)
        : _gamma(gamma), _hash_domain(size_t(ceil(double(n) * gamma))), _nelem(n),
          _num_thread(num_thread), _percent_elem_loaded_for_fastMode(perc_elem_loaded) {
        if (n == 0) {
            return;
        }

        _fastmode = (_percent_elem_loaded_for_fastMode > 0.0);
        setup();

        uint64_t offset = 0;
        for (int ii = 0; ii < _nb_levels; ii++) {
            // RAII: temp collision bitarray for this level
            _tempBitset = std::make_unique<bitVector>(_levels[ii].hash_domain);
            processLevel(input_range, ii);
            _levels[ii].bitset.clearCollisions(0, _levels[ii].hash_domain, _tempBitset.get());
            offset = _levels[ii].bitset.build_ranks(offset);
            _tempBitset.reset();
        }

        _lastbitsetrank = offset;
        std::vector<elem_t>().swap(setLevelFastmode);
        _built = true;
    }

    [[nodiscard]] uint64_t lookup(const elem_t& elem) const {
        if (!_built) [[unlikely]] {
            return ULLONG_MAX;
        }

        uint64_t non_minimal_hp, minimal_hp;

        hash_pair_t bbhash;
        int level;
        uint64_t level_hash = getLevel(bbhash, elem, &level);

        if (level == (_nb_levels - 1)) [[unlikely]] {
            auto in_final_map = _final_hash.find(elem);
            if (in_final_map == _final_hash.end()) {
                return ULLONG_MAX; // elem not in original key set
            }
            minimal_hp = in_final_map->second + _lastbitsetrank;
            return minimal_hp;
        } else [[likely]] {
            non_minimal_hp = fastrange64(level_hash, _levels[level].hash_domain);
        }
        minimal_hp = _levels[level].bitset.rank(non_minimal_hp);
        return minimal_hp;
    }

    [[nodiscard]] uint64_t nbKeys() const noexcept { return _nelem; }

    [[nodiscard]] uint64_t totalBitSize() const noexcept {
        uint64_t totalsizeBitset = 0;
        for (int ii = 0; ii < _nb_levels; ii++) {
            totalsizeBitset += _levels[ii].bitset.bitSize();
        }
        // unordered_map: ~42B per elem (uint64_t key); larger for other types
        uint64_t totalsize = totalsizeBitset + _final_hash.size() * 42 * 8;
        return totalsize;
    }

    template <typename Iterator>
    void pthread_processLevel(std::vector<elem_t>& buffer, std::shared_ptr<Iterator> shared_it,
                              std::shared_ptr<Iterator> until_p, int i) {
        (void)std::atomic_ref<int>(_nb_living).fetch_add(1, std::memory_order_relaxed);
        auto until = *until_p;
        uint64_t inbuff = 0;

        for (bool isRunning = true; isRunning;) {

            // safely copy n items into buffer
            _mutex.lock();
            for (; inbuff < NBBUFF && (*shared_it) != until; ++(*shared_it)) {
                buffer[inbuff] = *(*shared_it);
                inbuff++;
            }
            if ((*shared_it) == until) {
                isRunning = false;
            }
            _mutex.unlock();

            // do work on the n elems of the buffer
            for (uint64_t ii = 0; ii < inbuff; ii++) {
                elem_t val = buffer[ii];
                hash_pair_t bbhash;
                int level;
                uint64_t level_hash;
                getLevel(bbhash, val, &level, i);

                // insert into level i
                if (level == i) {
                    if (_fastmode && i == _fastModeLevel) {
                        uint64_t idxl2 = std::atomic_ref<uint64_t>(_idxLevelsetLevelFastmode)
                                             .fetch_add(1, std::memory_order_relaxed);
                        // if exceeds expected size, fall back to slow mode
                        // (shouldn't happen with a good hash and reasonable probabilities).
                        // atomic_ref avoids a formal data race on _fastmode — other
                        // OMP threads read it on the line above without synchronization.
                        if (idxl2 >= setLevelFastmode.size()) {
                            std::atomic_ref<bool>(_fastmode).store(false,
                                                                   std::memory_order_relaxed);
                        } else {
                            setLevelFastmode[idxl2] = val;
                        }
                    }

                    if (i == _nb_levels - 1) {
                        // last level reached: insert into exact hash
                        uint64_t hashidx = std::atomic_ref<uint64_t>(_hashidx).fetch_add(
                            1, std::memory_order_relaxed);
                        _mutex.lock();
                        _final_hash[val] = hashidx;
                        _mutex.unlock();
                    } else {
                        // compute next hash and insert into bitarray
                        if (level == 0) {
                            level_hash = _hasher.h0(bbhash, val);
                        } else if (level == 1) {
                            level_hash = _hasher.h1(bbhash, val);
                        } else {
                            level_hash = _hasher.next(bbhash);
                        }
                        insertIntoLevel(level_hash, i);
                    }
                }
            }

            inbuff = 0;
        }
    }

    void save(std::ostream& os) const {
        os.write(reinterpret_cast<char const*>(&_gamma), sizeof(_gamma));
        os.write(reinterpret_cast<char const*>(&_nb_levels), sizeof(_nb_levels));
        os.write(reinterpret_cast<char const*>(&_lastbitsetrank), sizeof(_lastbitsetrank));
        os.write(reinterpret_cast<char const*>(&_nelem), sizeof(_nelem));
        for (int ii = 0; ii < _nb_levels; ii++) {
            _levels[ii].bitset.save(os);
        }

        size_t final_hash_size = _final_hash.size();
        os.write(reinterpret_cast<char const*>(&final_hash_size), sizeof(size_t));

        for (const auto& [key, value] : _final_hash) {
            os.write(reinterpret_cast<char const*>(&key), sizeof(elem_t));
            os.write(reinterpret_cast<char const*>(&value), sizeof(uint64_t));
        }
    }

    void load(std::istream& is) {
        is.read(reinterpret_cast<char*>(&_gamma), sizeof(_gamma));
        is.read(reinterpret_cast<char*>(&_nb_levels), sizeof(_nb_levels));
        is.read(reinterpret_cast<char*>(&_lastbitsetrank), sizeof(_lastbitsetrank));
        is.read(reinterpret_cast<char*>(&_nelem), sizeof(_nelem));

        _levels.resize(_nb_levels);
        for (int ii = 0; ii < _nb_levels; ii++) {
            _levels[ii].bitset.load(is);
        }

        // recompute level sizes
        _proba_collision =
            1.0 - pow(((_gamma * (double)_nelem - 1) / (_gamma * (double)_nelem)), _nelem - 1);
        uint64_t previous_idx = 0;
        _hash_domain = (size_t)(ceil(double(_nelem) * _gamma));
        for (int ii = 0; ii < _nb_levels; ii++) {
            _levels[ii].idx_begin = previous_idx;
            _levels[ii].hash_domain =
                (((uint64_t)(_hash_domain * pow(_proba_collision, ii)) + 63) / 64) * 64;
            if (_levels[ii].hash_domain == 0) {
                _levels[ii].hash_domain = 64;
            }
            previous_idx += _levels[ii].hash_domain;
        }

        _final_hash.clear();
        size_t final_hash_size;
        is.read(reinterpret_cast<char*>(&final_hash_size), sizeof(size_t));

        for (unsigned int ii = 0; ii < final_hash_size; ii++) {
            elem_t key;
            uint64_t value;
            is.read(reinterpret_cast<char*>(&key), sizeof(elem_t));
            is.read(reinterpret_cast<char*>(&value), sizeof(uint64_t));
            _final_hash.emplace(std::move(key), value);
        }
        _built = true;
    }

  private:
    void setup() {
        if (_fastmode) {
            setLevelFastmode.resize(_percent_elem_loaded_for_fastMode * (double)_nelem);
        }

        _proba_collision =
            1.0 - pow(((_gamma * (double)_nelem - 1) / (_gamma * (double)_nelem)), _nelem - 1);

        _nb_levels = 25;
        _levels.resize(_nb_levels);

        uint64_t previous_idx = 0;
        for (int ii = 0; ii < _nb_levels; ii++) {
            _levels[ii].idx_begin = previous_idx;
            // round hash_domain up to nearest multiple of 64 (eases level clearing)
            _levels[ii].hash_domain =
                (((uint64_t)(_hash_domain * pow(_proba_collision, ii)) + 63) / 64) * 64;
            if (_levels[ii].hash_domain == 0) {
                _levels[ii].hash_domain = 64;
            }
            previous_idx += _levels[ii].hash_domain;
        }

        for (int ii = 0; ii < _nb_levels; ii++) {
            if (pow(_proba_collision, ii) < _percent_elem_loaded_for_fastMode) {
                _fastModeLevel = ii;
                break;
            }
        }
    }

    // Compute level reached for `val` and return the hash that landed there.
    //
    // Invariant on `bbhash` exit state — load-bearing for pthread_processLevel
    // which calls h0/h1/next once more to obtain the hash for `level`:
    //   - level == 0 (called with maxlevel=0): loop did not run, bbhash untouched
    //                → caller's h0(bbhash, val) yields the level-0 hash.
    //   - level == 1 (maxlevel=1): loop ran ii=0 (h0), bbhash[0]=h0_hash
    //                → caller's h1(bbhash, val) yields the level-1 hash.
    //   - level >= 2 (maxlevel=i): loop ran ii=0..i-1 → 1× h0, 1× h1, (i-2)× next
    //                → caller's next(bbhash) yields the (i-1)-th next, i.e. level-i hash.
    // Changing the call sequence (h0/h1/next) here will silently miscompute
    // insertion slots in the caller — keep the cascade exact.
    uint64_t getLevel(hash_pair_t& bbhash, const elem_t& val, int* res_level,
                      int maxlevel = 100) const {
        int level = 0;
        uint64_t hash_raw = 0;

        for (int ii = 0; ii < (_nb_levels - 1) && ii < maxlevel; ii++) {
            if (ii == 0) [[likely]] {
                hash_raw = _hasher.h0(bbhash, val);
            } else if (ii == 1) {
                hash_raw = _hasher.h1(bbhash, val);
            } else {
                hash_raw = _hasher.next(bbhash);
            }

            if (_levels[ii].get(hash_raw)) [[likely]] {
                break;
            }
            level++;
        }

        *res_level = level;
        return hash_raw;
    }

    void insertIntoLevel(uint64_t level_hash, int i) {
        uint64_t hashl = fastrange64(level_hash, _levels[i].hash_domain);
        if (_levels[i].bitset.atomic_test_and_set(hashl)) [[unlikely]] {
            (void)_tempBitset->atomic_test_and_set(hashl);
        }
    }

    // build level i in parallel via OpenMP
    template <typename Range> void processLevel(Range const& input_range, int i) {
        _levels[i].bitset = bitVector(_levels[i].hash_domain);

        _hashidx = 0;
        _idxLevelsetLevelFastmode = 0;
        _nb_living = 0;

        using it_type = decltype(input_range.begin());
        thread_args<Range> t_arg;
        t_arg.boophf = this;
        t_arg.range = &input_range;
        t_arg.it_p = std::static_pointer_cast<void>(std::make_shared<it_type>(input_range.begin()));
        t_arg.until_p =
            std::static_pointer_cast<void>(std::make_shared<it_type>(input_range.end()));
        t_arg.level = i;

        if (_fastmode && i >= (_fastModeLevel + 1)) {
            using fastmode_it_type = decltype(setLevelFastmode.begin());
            t_arg.it_p = std::static_pointer_cast<void>(
                std::make_shared<fastmode_it_type>(setLevelFastmode.begin()));
            t_arg.until_p = std::static_pointer_cast<void>(
                std::make_shared<fastmode_it_type>(setLevelFastmode.end()));

#pragma omp parallel num_threads(_num_thread)
            { thread_processLevel<elem_t, Hasher_t, Range, fastmode_it_type>(&t_arg); }
        } else {
#pragma omp parallel num_threads(_num_thread)
            {
                thread_processLevel<elem_t, Hasher_t, Range, decltype(input_range.begin())>(&t_arg);
            }
        }

        // shrink fastmode set to actual element count
        if (_fastmode && i == _fastModeLevel) {
            setLevelFastmode.resize(_idxLevelsetLevelFastmode);
        }
    }

  private:
    std::vector<level> _levels;
    int _nb_levels = 0;
    MultiHasher_t _hasher;
    std::unique_ptr<bitVector> _tempBitset;

    double _gamma = 2.0;
    uint64_t _hash_domain = 0;
    uint64_t _nelem = 0;
    std::unordered_map<elem_t, uint64_t, Hasher_t> _final_hash;
    int _nb_living = 0;
    int _num_thread = 1;
    uint64_t _hashidx = 0;
    double _proba_collision = 0.0;
    uint64_t _lastbitsetrank = 0;
    uint64_t _idxLevelsetLevelFastmode = 0;

    float _percent_elem_loaded_for_fastMode = 0.03f;
    bool _fastmode = false;
    std::vector<elem_t> setLevelFastmode;

    int _fastModeLevel = 0;
    bool _built = false;
    std::mutex _mutex;

    template <typename e, typename H, typename R, typename it>
    friend void thread_processLevel(thread_args<R>*);
};

////////////////////////////////////////////////////////////////
#pragma mark -
#pragma mark threading
////////////////////////////////////////////////////////////////

template <typename elem_t, typename Hasher_t, typename Range, typename it_type>
void thread_processLevel(thread_args<Range>* targ) {
    if (targ == nullptr) {
        return;
    }

    auto* obw = static_cast<mphf<elem_t, Hasher_t>*>(targ->boophf);
    int level = targ->level;
    std::vector<elem_t> buffer;
    buffer.resize(NBBUFF);

    // grab starting iterator (mutex-protected copy)
    obw->_mutex.lock();
    std::shared_ptr<it_type> startit = std::static_pointer_cast<it_type>(targ->it_p);
    std::shared_ptr<it_type> until_p = std::static_pointer_cast<it_type>(targ->until_p);
    obw->_mutex.unlock();

    obw->pthread_processLevel(buffer, startit, until_p, level);
}

} // namespace boomphf
