#pragma once

#include <memory>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include "xutils/numa_memory.hh"
#include "r2/src/common.hh"

namespace xstore {
namespace transport {

using namespace r2;

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
    std::mutex* mutexArray;
    int numa_node;

    ServerNumaState() : ludo_buckets_ptr(nullptr), 
                        packed_data_ptr(nullptr),
                        num_buckets(0), 
                        num_data_entries(0),
                        mutexArray(nullptr),
                        numa_node(-1) {}
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
