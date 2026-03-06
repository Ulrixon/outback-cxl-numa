#pragma once

#include <numa.h>
#include <numaif.h>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include "r2/src/logging.hh"

namespace xstore {
namespace numa {

/**
 * @brief NUMA memory region allocator
 * Replaces HugeRegion for NUMA-aware memory allocation
 */
class NumaRegion {
public:
    /**
     * @brief Create a NUMA memory region on specified node
     * @param size Size in bytes
     * @param node NUMA node ID (-1 for local node)
     * @return Pointer to NumaRegion or nullptr on failure
     */
    static std::shared_ptr<NumaRegion> create(size_t size, int node = -1) {
        if (numa_available() < 0) {
            LOG(4) << "NUMA is not available on this system";
            return nullptr;
        }

        // If node is -1, use the current node
        if (node == -1) {
            node = numa_node_of_cpu(sched_getcpu());
        }

        // Check if node is valid
        if (node < 0 || node >= numa_max_node() + 1) {
            LOG(4) << "Invalid NUMA node: " << node;
            return nullptr;
        }

        void* ptr = numa_alloc_onnode(size, node);
        if (!ptr) {
            LOG(4) << "Failed to allocate " << size << " bytes on NUMA node " << node;
            return nullptr;
        }

        LOG(2) << "Allocated " << size << " bytes on NUMA node " << node << " at address " << ptr;
        
        auto region = std::shared_ptr<NumaRegion>(new NumaRegion(ptr, size, node));
        return region;
    }

    /**
     * @brief Get the raw pointer to the memory region
     */
    void* get_ptr() const { return ptr_; }

    /**
     * @brief Get the size of the memory region
     */
    size_t get_size() const { return size_; }

    /**
     * @brief Get the NUMA node ID
     */
    int get_node() const { return node_; }

    /**
     * @brief Check if a pointer is within this region
     */
    bool contains(void* p) const {
        uintptr_t addr = reinterpret_cast<uintptr_t>(p);
        uintptr_t start = reinterpret_cast<uintptr_t>(ptr_);
        uintptr_t end = start + size_;
        return addr >= start && addr < end;
    }

    ~NumaRegion() {
        if (ptr_) {
            numa_free(ptr_, size_);
            LOG(3) << "Freed " << size_ << " bytes from NUMA node " << node_;
        }
    }

private:
    NumaRegion(void* ptr, size_t size, int node)
        : ptr_(ptr), size_(size), node_(node) {}

    void* ptr_;
    size_t size_;
    int node_;
};

/**
 * @brief NUMA-aware memory allocator
 * Replaces SimpleAllocator for NUMA memory
 */
class NumaAllocator {
public:
    NumaAllocator(std::shared_ptr<NumaRegion> region)
        : region_(region), offset_(0) {}

    /**
     * @brief Allocate memory from the NUMA region
     * @param size Size in bytes
     * @return Pointer to allocated memory or nullptr
     */
    void* alloc(size_t size) {
        // Align to 64-byte boundary (cache line)
        size_t aligned_size = (size + 63) & ~63;
        
        size_t current = offset_.fetch_add(aligned_size);
        if (current + aligned_size > region_->get_size()) {
            LOG(4) << "NumaAllocator: out of memory";
            return nullptr;
        }

        void* ptr = static_cast<char*>(region_->get_ptr()) + current;
        return ptr;
    }

    /**
     * @brief Get the underlying NUMA region
     */
    std::shared_ptr<NumaRegion> get_region() const { return region_; }

private:
    std::shared_ptr<NumaRegion> region_;
    std::atomic<size_t> offset_;
};

/**
 * @brief NUMA memory handle for shared access
 * Provides interface for accessing NUMA memory from different nodes
 */
struct NumaMemHandle {
    void* base_addr;
    size_t size;
    int node;

    NumaMemHandle() : base_addr(nullptr), size(0), node(-1) {}
    NumaMemHandle(void* addr, size_t sz, int n)
        : base_addr(addr), size(sz), node(n) {}

    /**
     * @brief Read data from NUMA memory
     */
    template<typename T>
    T read(size_t offset) const {
        if (offset + sizeof(T) > size) {
            throw std::out_of_range("NUMA read out of bounds");
        }
        T value;
        memcpy(&value, static_cast<char*>(base_addr) + offset, sizeof(T));
        return value;
    }

    /**
     * @brief Write data to NUMA memory
     */
    template<typename T>
    void write(size_t offset, const T& value) {
        if (offset + sizeof(T) > size) {
            throw std::out_of_range("NUMA write out of bounds");
        }
        memcpy(static_cast<char*>(base_addr) + offset, &value, sizeof(T));
    }

    /**
     * @brief Get pointer at offset
     */
    void* get_ptr(size_t offset = 0) const {
        if (offset > size) {
            return nullptr;
        }
        return static_cast<char*>(base_addr) + offset;
    }
};

/**
 * @brief Initialize NUMA library
 * Should be called at program startup
 */
inline void init_numa() {
    if (numa_available() < 0) {
        LOG(4) << "NUMA is not available on this system";
        throw std::runtime_error("NUMA not available");
    }
    LOG(2) << "NUMA initialized. Available nodes: " << (numa_max_node() + 1);
}

/**
 * @brief Get current NUMA node
 */
inline int get_current_numa_node() {
    return numa_node_of_cpu(sched_getcpu());
}

/**
 * @brief Bind current thread to a NUMA node
 */
inline void bind_to_numa_node(int node) {
    struct bitmask* nodemask = numa_allocate_nodemask();
    numa_bitmask_clearall(nodemask);
    numa_bitmask_setbit(nodemask, node);
    numa_bind(nodemask);
    numa_free_nodemask(nodemask);
    LOG(3) << "Thread bound to NUMA node " << node;
}

} // namespace numa
} // namespace xstore
