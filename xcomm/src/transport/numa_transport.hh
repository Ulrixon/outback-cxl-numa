#pragma once

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cstring>
#include <cctype>
#include <memory>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <string>
#include "xutils/numa_memory.hh"
#include "r2/src/common.hh"

namespace xstore {
namespace transport {

using namespace r2;

static constexpr uint64_t kNumaMetaMagic = 0x4F55544241434B4EULL; // "OUTBACKN"
static constexpr size_t kMaxNumaLanes = 64;

inline std::string numa_meta_name(const std::string& server_name) {
    return "/outback_numa_meta_" + server_name;
}

inline std::string numa_region_name(const std::string& server_name) {
    return "/outback_numa_region_" + server_name;
}

inline std::string make_numa_server_name(const std::string& server_addr) {
    std::string name;
    name.reserve(server_addr.size());
    for (char c : server_addr) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-') {
            name.push_back(c);
        } else {
            name.push_back('_');
        }
    }
    if (name.empty()) {
        name = "default";
    }
    return name;
}

inline size_t align_up(size_t value, size_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

struct alignas(64) SharedNumaRegistry {
    uint64_t magic;
    uint32_t version;
    uint32_t ready;

    int32_t numa_node;
    uint32_t reserved;

    size_t num_buckets;
    size_t num_data_entries;
    size_t region_size;

    size_t ludo_buckets_offset;
    size_t packed_array_offset;
    size_t lock_array_offset;

    uint32_t mem_threads;
    uint32_t reserved2;

    size_t next_free_index;
    size_t lane_next_free_index[kMaxNumaLanes];
};

inline bool map_numa_registry(const std::string& server_name,
                              bool create,
                              SharedNumaRegistry** registry,
                              int* fd_out) {
    if (!registry || !fd_out) {
        return false;
    }

    auto name = numa_meta_name(server_name);
    int flags = create ? (O_CREAT | O_RDWR) : O_RDWR;
    int fd = shm_open(name.c_str(), flags, 0666);
    if (fd < 0) {
        return false;
    }

    if (create && ftruncate(fd, sizeof(SharedNumaRegistry)) != 0) {
        close(fd);
        return false;
    }

    void* mapped = mmap(nullptr, sizeof(SharedNumaRegistry), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mapped == MAP_FAILED) {
        close(fd);
        return false;
    }

    if (create) {
        std::memset(mapped, 0, sizeof(SharedNumaRegistry));
        auto* meta = reinterpret_cast<SharedNumaRegistry*>(mapped);
        meta->magic = kNumaMetaMagic;
        meta->version = 1;
        meta->ready = 0;
        msync(mapped, sizeof(SharedNumaRegistry), MS_SYNC);
    }

    *registry = reinterpret_cast<SharedNumaRegistry*>(mapped);
    *fd_out = fd;
    return true;
}

inline bool create_shared_numa_region(const std::string& server_name,
                                      size_t size,
                                      int numa_node,
                                      void** base_out,
                                      int* fd_out) {
    if (!base_out || !fd_out) {
        return false;
    }

    auto name = numa_region_name(server_name);
    int fd = shm_open(name.c_str(), O_CREAT | O_RDWR, 0666);
    if (fd < 0) {
        return false;
    }

    if (ftruncate(fd, size) != 0) {
        close(fd);
        return false;
    }

    void* mapped = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mapped == MAP_FAILED) {
        close(fd);
        return false;
    }

    std::memset(mapped, 0, size);
    if (numa_available() >= 0 && numa_node >= 0) {
        numa_tonode_memory(mapped, size, numa_node);
    }

    *base_out = mapped;
    *fd_out = fd;
    return true;
}

inline bool open_shared_numa_region(const std::string& server_name,
                                    size_t size,
                                    void** base_out,
                                    int* fd_out) {
    if (!base_out || !fd_out) {
        return false;
    }

    auto name = numa_region_name(server_name);
    int fd = shm_open(name.c_str(), O_RDWR, 0666);
    if (fd < 0) {
        return false;
    }

    void* mapped = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mapped == MAP_FAILED) {
        close(fd);
        return false;
    }

    *base_out = mapped;
    *fd_out = fd;
    return true;
}

inline void numa_lock_byte(volatile uint8_t* lock_byte) {
    while (__sync_lock_test_and_set(lock_byte, 1)) {
        while (*lock_byte) {
            asm volatile("pause" ::: "memory");
        }
    }
}

inline void numa_unlock_byte(volatile uint8_t* lock_byte) {
    __sync_lock_release(lock_byte);
}

/**
 * @brief NUMA-based transport (replaces RDMA UD transport)
 * Provides direct memory access to NUMA-allocated server memory
 */
struct NumaTransport {
    numa::NumaMemHandle server_mem_handle;
    std::atomic<uint64_t> cor_id_counter{0};

    NumaTransport() = default;

    /**
     * @brief Connect to server memory region
     * @param mem_handle Handle to server's NUMA memory
     */
    auto connect(const numa::NumaMemHandle& handle) -> bool {
        server_mem_handle = handle;
        return true;
    }

    /**
     * @brief Check if connected
     */
    bool is_connected() const {
        return server_mem_handle.base_addr != nullptr;
    }

    /**
     * @brief Get next correlation ID
     */
    uint64_t next_cor_id() {
        return cor_id_counter.fetch_add(1);
    }
};

/**
 * @brief NUMA-based RPC operation (replaces RPCOp over RDMA)
 * Directly accesses server memory instead of sending messages
 */
struct NumaRPCOp {
    enum class OpType {
        GET,
        PUT,
        UPDATE,
        REMOVE,
        SCAN
    };

    OpType op_type;
    uint64_t cor_id;
    void* request_data;
    size_t request_size;
    void* reply_data;
    size_t reply_size;

    NumaRPCOp() : op_type(OpType::GET), cor_id(0), 
                  request_data(nullptr), request_size(0),
                  reply_data(nullptr), reply_size(0) {}

    auto set_op_type(OpType type) -> NumaRPCOp& {
        op_type = type;
        return *this;
    }

    auto set_corid(uint64_t id) -> NumaRPCOp& {
        cor_id = id;
        return *this;
    }

    auto set_request(void* data, size_t size) -> NumaRPCOp& {
        request_data = data;
        request_size = size;
        return *this;
    }

    auto set_reply(void* data, size_t size) -> NumaRPCOp& {
        reply_data = data;
        reply_size = size;
        return *this;
    }
};

/**
 * @brief Shared memory structure for server state
 * Contains pointers to actual data structures on NUMA node
 */
struct ServerNumaState {
    void* ludo_buckets_ptr;
    void* packed_data_ptr;
    size_t num_buckets;
    size_t num_data_entries;
    volatile uint8_t* lock_array;
    int numa_node;

    SharedNumaRegistry* shared_meta;
    void* shared_region_base;
    int shared_region_fd;
    int shared_meta_fd;

    ServerNumaState() : ludo_buckets_ptr(nullptr), 
                        packed_data_ptr(nullptr),
                        num_buckets(0), 
                        num_data_entries(0),
                        lock_array(nullptr),
                        numa_node(-1),
                        shared_meta(nullptr),
                        shared_region_base(nullptr),
                        shared_region_fd(-1),
                        shared_meta_fd(-1) {}
};

/**
 * @brief Global server state manager
 * Manages shared access to server NUMA memory
 */
class NumaServerManager {
public:
    static NumaServerManager& instance() {
        static NumaServerManager mgr;
        return mgr;
    }

    void register_server_state(const std::string& name, ServerNumaState* state) {
        std::lock_guard<std::mutex> lock(mutex_);
        server_states_[name] = state;
    }

    ServerNumaState* get_server_state(const std::string& name) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = server_states_.find(name);
        if (it != server_states_.end()) {
            return it->second;
        }
        return nullptr;
    }

    void unregister_server_state(const std::string& name) {
        std::lock_guard<std::mutex> lock(mutex_);
        server_states_.erase(name);
    }

private:
    NumaServerManager() = default;
    std::unordered_map<std::string, ServerNumaState*> server_states_;
    std::mutex mutex_;
};

/**
 * @brief Result wrapper for NUMA operations
 */
template<typename T = void>
struct NumaResult {
    enum class Status {
        Ok,
        NotFound,
        OutOfMemory,
        InvalidArg,
        Error
    };

    Status status;
    T value;

    NumaResult() : status(Status::Error) {}
    NumaResult(Status s) : status(s) {}
    NumaResult(Status s, const T& v) : status(s), value(v) {}

    bool is_ok() const { return status == Status::Ok; }
    operator bool() const { return is_ok(); }
};

// Specialization for void
template<>
struct NumaResult<void> {
    enum class Status {
        Ok,
        NotFound,
        OutOfMemory,
        InvalidArg,
        Error
    };

    Status status;

    NumaResult() : status(Status::Error) {}
    NumaResult(Status s) : status(s) {}

    bool is_ok() const { return status == Status::Ok; }
    operator bool() const { return is_ok(); }
};

/**
 * @brief NUMA memory barrier utilities
 */
inline void numa_read_barrier() {
    asm volatile("lfence" ::: "memory");
}

inline void numa_write_barrier() {
    asm volatile("sfence" ::: "memory");
}

inline void numa_full_barrier() {
    asm volatile("mfence" ::: "memory");
}

} // namespace transport
} // namespace xstore
