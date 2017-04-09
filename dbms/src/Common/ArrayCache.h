#pragma once

#include <atomic>
#include <mutex>
#include <list>
#include <memory>
#include <unordered_map>
#include <sys/mman.h>
#include <boost/intrusive/list.hpp>
#include <boost/intrusive/set.hpp>
#include <boost/noncopyable.hpp>

#include <Common/Exception.h>


namespace DB
{
    namespace ErrorCodes
    {
        extern const int CANNOT_ALLOCATE_MEMORY;
        extern const int CANNOT_MUNMAP;
    }
}


/** Cache for variable length memory regions (contiguous arrays of bytes).
  * Example: cache for data read from disk, cache for decompressed data, etc.
  * It combines cache and allocator: allocates memory by itself without use of malloc/new.
  *
  * Motivation:
  * - cache has specified memory usage limit and we want this limit to include allocator fragmentation overhead;
  * - the cache overcomes memory fragmentation by cache eviction;
  * - there is no sense for reclaimed memory regions to be cached internally by usual allocator (malloc/new);
  * - by allocating memory directly with mmap, we could place it in virtual address space far
  *   from other (malloc'd) memory - this helps debugging memory stomping bugs
  *   (your program will likely hit unmapped memory and get segfault rather than silent cache corruption)
  *
  * Implementation:
  *
  * Cache is represented by list of mmapped chunks.
  * Each chunk holds free and occupied memory regions. Contiguous free regions are always coalesced.
  *
  * Each region is linked by following metadata structures:
  * 1. LRU list - to find next region for eviction.
  * 2. Adjacency list - to find neighbour free regions to coalesce on eviction.
  * 3. Size multimap - to find free region with at least requested size.
  * 4. Key map - to find element by key.
  *
  * Each region has:
  * - size;
  * - refcount: region could be evicted only if it is not used anywhere;
  * - chunk address: to check if regions are from same chunk.
  *
  * During insertion, each key is locked - to avoid parallel initialization of regions for same key.
  *
  * On insertion, we search for free region of at least requested size.
  * If nothing was found, we evict oldest unused region and try again.
  *
  * Metadata is allocated by usual allocator and its memory usage is not accounted.
  *
  * Caveats:
  * - cache is not NUMA friendly.
  */

template <typename Key, typename Payload>
class ArrayCache : private boost::noncopyable
{
private:
    struct LRUListTag;
    struct AdjacencyListTag;
    struct SizeMultimapTag;
    struct KeyMapTag;

    using LRUListHook = boost::intrusive::list_base_hook<boost::intrusive::tag<LRUListTag>>;
    using AdjacencyListHook = boost::intrusive::list_base_hook<boost::intrusive::tag<AdjacencyListTag>>;
    using SizeMultimapHook = boost::intrusive::set_base_hook<boost::intrusive::tag<SizeMultimapTag>>;
    using KeyMapHook = boost::intrusive::set_base_hook<boost::intrusive::tag<KeyMapTag>>;

    struct RegionMetadata : public LRUListHook, AdjacencyListHook, SizeMultimapHook, KeyMapHook
    {
        Key key;
        Payload payload;

        void * ptr;
        size_t size;
        std::atomic<size_t> refcount {0};
        void * chunk;

        bool operator< (const RegionMetadata & other) const { return size < other.size; }

        bool isFree() const { return SizeMultimapHook::is_linked(); }

        static RegionMetadata & create()
        {
            return *(new RegionMetadata);
        }

        void destroy()
        {
            delete this;
        }

    private:
         RegionMetadata() = default;
         ~RegionMetadata() = default;
    };

    struct RegionCompareBySize
    {
        bool operator() (const RegionMetadata & a, const RegionMetadata & b) const { return a.size < b.size; }
        bool operator() (const RegionMetadata & a, size_t size) const { return a.size < size; }
    };

    struct RegionCompareByKey
    {
        bool operator() (const RegionMetadata & a, const RegionMetadata & b) const { return a.key < b.key; }
    };

    using LRUList = boost::intrusive::list<RegionMetadata, LRUListHook>;
    using AdjacencyList = boost::intrusive::list<RegionMetadata, AdjacencyListHook>;
    using SizeMultimap = boost::intrusive::multiset<RegionMetadata, RegionCompareBySize, SizeMultimapHook>;
    using KeyMap = boost::intrusive::set<RegionMetadata, RegionCompareByKey, SizeMultimapHook>;

    /** Invariants:
      * adjacency_list contains all regions
      * size_multimap contains free regions
      * key_map contains allocated regions
      * lru_list contains allocated regions, that are not in use
      */

    LRUList lru_list;
    AdjacencyList adjacency_list;
    SizeMultimap size_multimap;
    KeyMap key_map;

    std::mutex mutex;


    struct Chunk : private boost::noncopyable
    {
        void * ptr;
        size_t size;

        Chunk(size_t size_) : size(size_)
        {
            ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (MAP_FAILED == ptr)
                DB::throwFromErrno("Allocator: Cannot mmap.", DB::ErrorCodes::CANNOT_ALLOCATE_MEMORY);
        }

        ~Chunk()
        {
            if (0 != munmap(ptr, size))
                DB::throwFromErrno("Allocator: Cannot munmap.", DB::ErrorCodes::CANNOT_MUNMAP);
        }
    };

    using Chunks = std::list<Chunk>;
    Chunks chunks;

    size_t total_chunks_size = 0;
    size_t total_allocated_size = 0;
    size_t total_size_currently_initialized = 0;
    size_t total_size_in_use = 0;

    /// Max size of cache.
    const size_t max_total_size;

    /// We will allocate memory in chunks of at least that size.
    /// 64 MB makes mmap overhead comparable to memory throughput.
    static constexpr size_t min_chunk_size = 64 * 1024 * 1024;

    /// Cache stats.
    size_t hits;
    size_t misses;


    /// Holds region as in use. Regions in use could not be evicted from cache.
    /// In constructor, increases refcount and if it becomes greater than zero, removes region from lru_list.
    /// In destructor, decreases refcount and if it becomes zero, insert region to lru_list.
    struct Holder : private boost::noncopyable
    {
        Holder(ArrayCache & cache_, RegionMetadata & region_) : cache(cache_), region(region_)
        {
            if (++region.refcount == 1)
                cache.lru_list.erase(region);
        }

        ~Holder()
        {
            if (--region.refcount == 0)
                cache.lru_list.push_back(region);
        }

        ArrayCache & cache;
        RegionMetadata & region;
    };

    using HolderPtr = std::shared_ptr<Holder>;


    /// Represents pending insertion attempt.
    struct InsertToken
    {
        InsertToken(ArrayCache & cache_) : cache(cache_) {}

        std::mutex mutex;
        bool cleaned_up = false; /// Protected by the token mutex
        HolderPtr value; /// Protected by the token mutex

        ArrayCache & cache;
        size_t refcount = 0; /// Protected by the cache mutex
    };

    using InsertTokens = std::unordered_map<Key, std::shared_ptr<InsertToken>>;
    InsertTokens insert_tokens;

    /// This class is responsible for removing used insert tokens from the insert_tokens map.
    /// Among several concurrent threads the first successful one is responsible for removal. But if they all
    /// fail, then the last one is responsible.
    struct InsertTokenHolder
    {
        const Key * key = nullptr;
        std::shared_ptr<InsertToken> token;
        bool cleaned_up = false;

        InsertTokenHolder() = default;

        void acquire(const Key * key_, const std::shared_ptr<InsertToken> & token_, std::lock_guard<std::mutex> & cache_lock)
        {
            key = key_;
            token = token_;
            ++token->refcount;
        }

        void cleanup(std::lock_guard<std::mutex> & token_lock, std::lock_guard<std::mutex> & cache_lock)
        {
            token->cache.insert_tokens.erase(*key);
            token->cleaned_up = true;
            cleaned_up = true;
        }

        ~InsertTokenHolder()
        {
            if (!token)
                return;

            if (cleaned_up)
                return;

            std::lock_guard<std::mutex> token_lock(token->mutex);

            if (token->cleaned_up)
                return;

            std::lock_guard<std::mutex> cache_lock(token->cache.mutex);

            --token->refcount;
            if (token->refcount == 0)
                cleanup(token_lock, cache_lock);
        }
    };

    friend struct InsertTokenHolder;


    static constexpr size_t page_size = 4096;
    static size_t roundUpToPageSize(size_t x)
    {
        return (x + (page_size - 1)) / page_size * page_size;
    }


    /// Evict one region from cache and return it, coalesced with neighbours.
    /// If nothing to evict, returns nullptr.
    RegionMetadata * evictOne()
    {
        if (lru_list.empty())
            return nullptr;

        RegionMetadata & evicted_region = lru_list.front();

        lru_list.pop_front();
        key_map.erase(evicted_region);

        auto adjacency_list_it = adjacency_list.iterator_to(evicted_region);

        bool has_free_region_at_left = false;
        bool has_free_region_at_right = false;

        auto left_it = adjacency_list_it;
        auto right_it = adjacency_list_it;

        if (left_it != adjacency_list.begin())
        {
            --left_it;
            if (left_it->chunk == evicted_region.chunk && left_it->isFree())
                has_free_region_at_left = true;
        }

        ++right_it;
        if (right_it != adjacency_list.end())
        {
            if (right_it->chunk == evicted_region.chunk && right_it->isFree())
                has_free_region_at_right = true;
        }

        /// Coalesce free regions.
        if (has_free_region_at_left)
        {
            evicted_region.size += left_it->size;
            evicted_region.ptr -= left_it->size;
            size_multimap.erase(*left_it);
            adjacency_list.erase(left_it);
            left_it->destroy();
        }

        if (has_free_region_at_right)
        {
            evicted_region.size += right_it->size;
            size_multimap.erase(*right_it);
            adjacency_list.erase(right_it);
            right_it->destroy();
        }

        size_multimap.insert(evicted_region);
        return &evicted_region;
    }


    /// Precondition: free_region.size > size.
    RegionMetadata & splitFreeRegion(RegionMetadata & free_region, size_t size)
    {
        RegionMetadata & allocated_region = RegionMetadata::create();
        allocated_region.ptr = free_region.ptr;
        allocated_region.chunk = free_region.ptr;
        allocated_region.size = size;

        size_multimap.erase(free_region);
        free_region.size -= size;
        free_region.ptr += size;
        size_multimap.insert(free_region);

        adjacency_list.insert(adjacency_list.iterator_to(free_region), allocated_region);
        return allocated_region;
    }


    RegionMetadata * allocate(size_t size)
    {
        /// Look up to size multimap to find free region of specified size.
        auto it = size_multimap.lower_bound(size, RegionCompareBySize());
        if (size_multimap.end() != it)
        {
            RegionMetadata & res = *it;
            size_multimap.erase(res);
            return &res;
        }

        /// If nothing was found and total size of allocated chunks plus required size is lower than maximum,
        ///  allocate a new chunk.
        size_t required_chunk_size = std::max(min_chunk_size, roundUpToPageSize(size));
        if (total_chunks_size + required_chunk_size <= max_total_size)
        {
            chunks.emplace_back(required_chunk_size);
            Chunk & chunk = chunks.back();

            /// Create free region spanning through chunk.
            RegionMetadata & free_region = RegionMetadata::create();

            free_region.ptr = chunk.ptr;
            free_region.chunk = chunk.ptr;
            free_region.size = chunk.size;

            adjacency_list.push_back(free_region);

            /// Cut region from the beginning of free space.
            if (chunk.size > size)
            {
                size_multimap.insert(free_region);
                return &splitFreeRegion(free_region, size);
            }
            else
                return &free_region;
        }

        /// Evict something from cache and continue.
        while (true)
        {
            RegionMetadata * res = evictOne();

            /// Nothing to evict. All cache is full and in use - cannot allocate memory.
            if (!res)
                return nullptr;

            /// Not enough. Evict more.
            if (res->size < size)
                continue;

            /// Cut region from the beginning of free space.
            if (res->size > size)
                res = &splitFreeRegion(*res, size);

            size_multimap.erase(*res);
            return res;
        }
    }


public:
    ~ArrayCache()
    {
        std::lock_guard<std::mutex> cache_lock(mutex);

        for (auto elem : adjacency_list)
            elem.destroy();
    }


    /// If the value for the key is in the cache, returns it.
    ///
    /// If it is not, calls 'get_size' to obtain required size of storage for key,
    ///  then allocates storage and call 'initialize' for necessary initialization before data from cache could be used.
    ///
    /// Only one of several concurrent threads calling this method will call get_size or initialize,
    /// others will wait for that call to complete and will use its result (this helps prevent cache stampede).
    ///
    /// Exceptions occuring in callbacks will be propagated to the caller.
    /// Another thread from the set of concurrent threads will then try to call its callbacks etc.
    ///
    /// Returns std::pair of the cached value and a bool indicating whether the value was produced during this call.
    template <typename GetSizeFunc, typename InitializeFunc>
    std::pair<HolderPtr, bool> getOrSet(const Key & key, GetSizeFunc && get_size, InitializeFunc && initialize)
    {
        InsertTokenHolder token_holder;
        {
            std::lock_guard<std::mutex> cache_lock(mutex);

            auto val = getImpl(key, cache_lock);
            if (val)
            {
                ++hits;
                return std::make_pair(val, false);
            }

            auto & token = insert_tokens[key];
            if (!token)
                token = std::make_shared<InsertToken>(*this);

            token_holder.acquire(&key, token, cache_lock);
        }

        InsertToken * token = token_holder.token.get();

        std::lock_guard<std::mutex> token_lock(token->mutex);

        token_holder.cleaned_up = token->cleaned_up;

        if (token->value)
        {
            /// Another thread already produced the value while we waited for token->mutex.
            ++hits;
            return std::make_pair(token->value, false);
        }

        ++misses;
        token->value = load_func();

        std::lock_guard<std::mutex> cache_lock(mutex);

        /// Insert the new value only if the token is still in present in insert_tokens.
        /// (The token may be absent because of a concurrent reset() call).
        auto token_it = insert_tokens.find(key);
        if (token_it != insert_tokens.end() && token_it->second.get() == token)
            setImpl(key, token->value, cache_lock);

        if (!token->cleaned_up)
            token_holder.cleanup(token_lock, cache_lock);

        return std::make_pair(token->value, true);
    }
};
