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

// ─── Constants ───────────────────────────────────────────────────────────────

static constexpr uint64_t kNumaMetaMagic  = 0x4F55544241434B4EULL; // "OUTBACKN"
static constexpr size_t   kMaxNumaLanes   = 64;
static constexpr size_t   kMaxClients     = 256;
static constexpr size_t   kMaxEpochSlots  = 16;   // versions 0–15; version 0 unused

// ─── Naming helpers ──────────────────────────────────────────────────────────

inline std::string numa_meta_name(const std::string& server_name) {
    return "/outback_numa_meta_" + server_name;
}

// version == 0 → legacy / default name (backwards compat)
inline std::string numa_region_name(const std::string& server_name,
                                    uint64_t version = 0) {
    if (version == 0) return "/outback_numa_region_" + server_name;
    return "/outback_numa_region_" + server_name + "_v" + std::to_string(version);
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
    if (name.empty()) name = "default";
    return name;
}

inline size_t align_up(size_t value, size_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

// ─── Operation status (returned by server to client) ─────────────────────────

enum class OpStatus : uint32_t {
    OK             = 0,
    NOT_FOUND      = 1,
    PRE_RESIZE     = 2,  // Soft threshold; operation allowed but resize approaching
    RETRY_LATER    = 3,  // Structural mutation blocked during COPYING/DRAIN_OLD
    DEFERRED       = 4,  // Insert/Delete enqueued for later replay
    STALE_VERSION  = 5,  // Client mapping is behind; remap and retry
    INTERNAL_ERROR = 6
};

// ─── Server-side resize state machine ────────────────────────────────────────

enum class ResizeState : uint32_t {
    NORMAL          = 0,
    PRE_RESIZE      = 1,  // s_slow crossed; preparing but not yet building new region
    COPYING         = 2,  // Building + populating new versioned region
    READY_TO_SWITCH = 3,  // New region published; waiting for client acks
    DRAIN_OLD       = 4,  // Replaying deferred mutations into new region; draining old
    GC_PENDING      = 5   // Old region safe to reclaim after grace period
};

// ─── Per-client liveness (heartbeat-based zombie detection) ──────────────────

struct ClientLiveness {
    std::atomic<uint64_t> last_heartbeat_ns;    // nanosecond timestamp
    std::atomic<uint64_t> last_seen_generation;
    std::atomic<uint32_t> active;               // 1 = registered, 0 = gone
    uint32_t              _pad;
};

// ─── Per-epoch metadata (one slot per versioned region) ──────────────────────

struct ResizeEpoch {
    // Identity
    uint64_t version;
    uint64_t publish_ts_ns;

    // Region geometry
    size_t region_size;
    size_t num_buckets;
    size_t num_data_entries;

    // Offsets within the versioned shm region
    size_t packed_array_offset;
    size_t ludo_buckets_offset;
    size_t lock_array_offset;

    // Extendible hashing metadata
    uint32_t global_depth;
    uint32_t directory_size;
    uint32_t directory_version;
    uint32_t reserved0;

    // Coordination counters (lock-free on x86-64)
    std::atomic<uint64_t> clients_pending_ack;
    std::atomic<uint64_t> active_readers;
    std::atomic<uint64_t> active_writers;
    std::atomic<uint32_t> published;        // 1 after release fence; readable by clients
    std::atomic<uint32_t> cleanup_allowed;  // 1 when GC may reclaim this epoch's region
};

// ─── Shared registry (permanently mapped control plane) ──────────────────────
//
// Lives in /outback_numa_meta_<server_name>.
// Never resized in place.  Epoch entries are append-only until cleanup.
// Clients must re-read whenever 'generation' changes.

struct alignas(64) SharedNumaRegistry {
    // ── Immutable identity
    uint64_t magic;
    uint32_t protocol_version;

    // ── Resize state machine
    std::atomic<uint32_t> resize_state;        // ResizeState enum

    // ── Version tracking
    std::atomic<uint64_t> current_version;     // Active epoch index (1-based)
    std::atomic<uint64_t> previous_version;    // Epoch being drained / GC'd

    // ── Fast-path change signal
    // Incremented on every state transition or epoch publication so clients
    // can poll cheaply without reading all epoch metadata on every op.
    std::atomic<uint64_t> generation;

    // ── Server readiness (1 = ready, 0 = not ready / shutting down)
    uint64_t ready;

    // ── NUMA topology
    int32_t  numa_node;
    uint32_t _pad0;

    // ── Resize thresholds (entry counts)
    uint64_t s_slow;   // Soft: enter PRE_RESIZE
    uint64_t s_stop;   // Hard: block Insert/Delete

    // ── Client bookkeeping
    std::atomic<uint64_t> registered_clients;
    std::atomic<uint64_t> deferred_insert_count;
    std::atomic<uint64_t> deferred_delete_count;

    // ── Extendible hashing global state
    std::atomic<uint32_t> global_depth;
    std::atomic<uint32_t> directory_version;

    // ── Epoch table
    // epoch[current_version % kMaxEpochSlots] is the active epoch.
    // Slot 0 is unused so version numbers are 1-based.
    ResizeEpoch epoch[kMaxEpochSlots];

    // ── Per-client liveness table
    ClientLiveness clients[kMaxClients];

    // ── Allocation lanes (lock-free, per-lane free-list index)
    uint32_t mem_threads;        // Number of active lanes
    uint32_t _pad1;
    size_t   lane_next_free_index[kMaxNumaLanes];
};

// ─── Convenience: resolve the current active epoch ───────────────────────────

inline const ResizeEpoch& current_epoch(const SharedNumaRegistry& reg) {
    uint64_t v = reg.current_version.load(std::memory_order_acquire);
    return reg.epoch[v % kMaxEpochSlots];
}
inline ResizeEpoch& current_epoch(SharedNumaRegistry& reg) {
    uint64_t v = reg.current_version.load(std::memory_order_acquire);
    return reg.epoch[v % kMaxEpochSlots];
}

// ─── Shared-memory registry mapping ─────────────────────────────────────────

inline bool map_numa_registry(const std::string& server_name,
                              bool create,
                              SharedNumaRegistry** registry,
                              int* fd_out) {
    if (!registry || !fd_out) return false;

    auto name  = numa_meta_name(server_name);
    int  flags = create ? (O_CREAT | O_RDWR) : O_RDWR;
    int  fd    = shm_open(name.c_str(), flags, 0666);
    if (fd < 0) return false;

    if (create && ftruncate(fd, sizeof(SharedNumaRegistry)) != 0) {
        close(fd); return false;
    }

    void* mapped = mmap(nullptr, sizeof(SharedNumaRegistry),
                        PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mapped == MAP_FAILED) { close(fd); return false; }

    if (create) {
        std::memset(mapped, 0, sizeof(SharedNumaRegistry));
        auto* meta = reinterpret_cast<SharedNumaRegistry*>(mapped);
        meta->magic            = kNumaMetaMagic;
        meta->protocol_version = 1;
        meta->resize_state.store(static_cast<uint32_t>(ResizeState::NORMAL),
                                 std::memory_order_relaxed);
        meta->current_version.store(1,  std::memory_order_relaxed);
        meta->previous_version.store(0, std::memory_order_relaxed);
        meta->generation.store(0,       std::memory_order_relaxed);
        meta->ready = 0;
        msync(mapped, sizeof(SharedNumaRegistry), MS_SYNC);
    }

    *registry = reinterpret_cast<SharedNumaRegistry*>(mapped);
    *fd_out   = fd;
    return true;
}

// ─── Versioned shm data region create / open ─────────────────────────────────

inline bool create_shared_numa_region(const std::string& server_name,
                                      size_t   size,
                                      int      numa_node,
                                      uint64_t version,
                                      void**   base_out,
                                      int*     fd_out) {
    if (!base_out || !fd_out) return false;

    auto name = numa_region_name(server_name, version);
    int  fd   = shm_open(name.c_str(), O_CREAT | O_RDWR, 0666);
    if (fd < 0) return false;

    if (ftruncate(fd, size) != 0) { close(fd); return false; }

    void* mapped = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mapped == MAP_FAILED) { close(fd); return false; }

    std::memset(mapped, 0, size);
    if (numa_available() >= 0 && numa_node >= 0) {
        numa_tonode_memory(mapped, size, numa_node);
    }

    *base_out = mapped;
    *fd_out   = fd;
    return true;
}

inline bool open_shared_numa_region(const std::string& server_name,
                                    size_t   size,
                                    uint64_t version,
                                    void**   base_out,
                                    int*     fd_out) {
    if (!base_out || !fd_out) return false;

    auto name = numa_region_name(server_name, version);
    int  fd   = shm_open(name.c_str(), O_RDWR, 0666);
    if (fd < 0) return false;

    void* mapped = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mapped == MAP_FAILED) { close(fd); return false; }

    *base_out = mapped;
    *fd_out   = fd;
    return true;
}

// Helper to unlink an old versioned region file
inline void unlink_numa_region(const std::string& server_name, uint64_t version) {
    shm_unlink(numa_region_name(server_name, version).c_str());
}

// ─── Byte-granularity bucket lock helpers ────────────────────────────────────

inline void numa_lock_byte(volatile uint8_t* lock_byte) {
    while (__sync_lock_test_and_set(lock_byte, 1)) {
        while (*lock_byte) { asm volatile("pause" ::: "memory"); }
    }
}
inline void numa_unlock_byte(volatile uint8_t* lock_byte) {
    __sync_lock_release(lock_byte);
}

// ─── NUMA transport stub ─────────────────────────────────────────────────────

struct NumaTransport {
    numa::NumaMemHandle   server_mem_handle;
    std::atomic<uint64_t> cor_id_counter{0};

    NumaTransport() = default;
    auto connect(const numa::NumaMemHandle& handle) -> bool {
        server_mem_handle = handle; return true;
    }
    bool     is_connected() const { return server_mem_handle.base_addr != nullptr; }
    uint64_t next_cor_id()        { return cor_id_counter.fetch_add(1); }
};

// ─── RPC operation type stub ─────────────────────────────────────────────────

struct NumaRPCOp {
    enum class OpType { GET, PUT, UPDATE, REMOVE, SCAN };
    OpType   op_type      = OpType::GET;
    uint64_t cor_id       = 0;
    void*    request_data = nullptr;
    size_t   request_size = 0;
    void*    reply_data   = nullptr;
    size_t   reply_size   = 0;
    auto set_op_type(OpType t)          -> NumaRPCOp& { op_type = t;      return *this; }
    auto set_corid(uint64_t id)         -> NumaRPCOp& { cor_id = id;      return *this; }
    auto set_request(void* d, size_t s) -> NumaRPCOp& { request_data = d; request_size = s; return *this; }
    auto set_reply(void* d, size_t s)   -> NumaRPCOp& { reply_data = d;   reply_size   = s; return *this; }
};

// ─── Server-local state (process-private; not in shared memory) ──────────────

struct ServerNumaState {
    // Pointers to live data structures (server only)
    void*             ludo_buckets_ptr  = nullptr;
    void*             packed_data_ptr   = nullptr;
    size_t            num_buckets       = 0;
    size_t            num_data_entries  = 0;
    volatile uint8_t* lock_array        = nullptr;
    int               numa_node         = -1;

    // Shared-memory handles (version 1 region at startup)
    SharedNumaRegistry* shared_meta       = nullptr;
    void*               shared_region_base = nullptr;
    int                 shared_region_fd   = -1;
    int                 shared_meta_fd     = -1;
};

// ─── In-process server manager (fast path when client and server co-locate) ──

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
        return (it != server_states_.end()) ? it->second : nullptr;
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

// ─── Result wrapper ───────────────────────────────────────────────────────────

template<typename T = void>
struct NumaResult {
    enum class Status { Ok, NotFound, OutOfMemory, InvalidArg, Error };
    Status status = Status::Error;
    T      value  = {};
    NumaResult() = default;
    NumaResult(Status s) : status(s) {}
    NumaResult(Status s, const T& v) : status(s), value(v) {}
    bool is_ok() const { return status == Status::Ok; }
    operator bool() const { return is_ok(); }
};
template<>
struct NumaResult<void> {
    enum class Status { Ok, NotFound, OutOfMemory, InvalidArg, Error };
    Status status = Status::Error;
    NumaResult() = default;
    NumaResult(Status s) : status(s) {}
    bool is_ok() const { return status == Status::Ok; }
    operator bool() const { return is_ok(); }
};

// ─── Memory barriers ─────────────────────────────────────────────────────────

inline void numa_read_barrier()  { asm volatile("lfence" ::: "memory"); }
inline void numa_write_barrier() { asm volatile("sfence" ::: "memory"); }
inline void numa_full_barrier()  { asm volatile("mfence" ::: "memory"); }

// ─── Resize mutation interception globals ─────────────────────────────────────
//
// Set by ResizeOrchestrator::start() so that op-handler code in
// outback_server_numa.hh can check/defer mutations without depending on the
// ResizeOrchestrator class definition (which it cannot include without
// creating a circular dependency).
//
// g_structural_mutations_blocked:
//   true  when resize_state is COPYING, READY_TO_SWITCH, or DRAIN_OLD.
//   Op handlers must not perform Insert/Delete; they should call the deferred
//   function pointers below and return DEFERRED to the client.
//
// g_defer_insert_fn / g_defer_delete_fn:
//   Filled in by the orchestrator on start(); point to lambda wrappers that
//   call ResizeOrchestrator::defer_insert()/defer_delete() via opaque pointer.
inline std::atomic<bool> g_structural_mutations_blocked{false};
inline void*             g_resize_orch_ptr   = nullptr;
inline bool (*g_defer_insert_fn)(void* orch, uint64_t key, uint64_t val) = nullptr;
inline bool (*g_defer_delete_fn)(void* orch, uint64_t key) = nullptr;

} // namespace transport
} // namespace xstore
