#ifndef __DISTORE__MEMORY__COMPUTE_NODE__COMPUTE_NODE__
#define __DISTORE__MEMORY__COMPUTE_NODE__COMPUTE_NODE__
#include "node/node.hpp"
#include "memory/memory.hpp"
#include "memory/remote_memory/remote_memory.hpp"
#include "memory/memory_node/memory_node.hpp"
#include "rdma_util/rdma_util.hpp"
#include "misc/misc.hpp"

#include <unordered_map>
#include <thread>
#include <mutex>
#include <fstream>

namespace DiStore {
    namespace Memory {

        namespace Enums {
            enum class MemoryAllocationStatus {
                Ok,
                NoMemory,
                EmptyPage,
                EmptyPageGroup,
            };
        }

        using namespace RDMAUtil;

        enum AllocationClass {
            Chunk16,
            Chunk32,
            Chunk64,
            Chunk128,
            Chunk256,
            Chunk512,
            Chunk1024,
            Chunk2048,
            Chunk4096,
            ChunkUnknown,
        };

        const static AllocationClass allocation_class_map[] = {
            Chunk16,
            Chunk32,
            Chunk64,
            Chunk128,
            Chunk256,
            Chunk512,
            Chunk1024,
            Chunk2048,
            Chunk4096,
            ChunkUnknown,
        };

        const static size_t allocation_class_size_map[] = {
            16, 32, 64, 128, 256, 512, 1024, 2048, 4096
        };

        struct PageDescriptor {
            byte_t empty_slots;
            byte_t allocation_class : 5;
            byte_t synced : 3;
            byte_t offset;

            auto initialize(AllocationClass ac) -> void {
                allocation_class = ac;

                auto size = allocation_class_size_map[ac];
                empty_slots = Constants::MEMORY_PAGE_SIZE / size;

                synced = 0;
                offset = 0;
            }

            auto clear() -> void {
                empty_slots = 0;
                allocation_class = 0;
                synced = 0;
                offset = 0;
            }
        } __attribute__((packed));

        struct PageMirror {
            PageDescriptor desc;
            uint64_t page_id : 40;

            // this field can be calculated, but explicited stored for simplicity
            RemotePointer page_base;

            // allocate always succeeds because caller will guarantee page validity
            auto allocate() -> RemotePointer;
            auto available() -> bool;
            auto free(RemotePointer ptr) -> bool;
        } __attribute__((packed));

        struct PageGroup {
            PageMirror *pages[Constants::PAGEGROUP_NO];

            // caller will use available() to ensure page group offers sufficient memory
            auto allocate(AllocationClass ac) -> RemotePointer;
            auto available(AllocationClass ac) -> Enums::MemoryAllocationStatus;
            auto free(RemotePointer) -> bool;

            auto dump() const noexcept -> void;
        };

        struct Segment {
            RemotePointer seg;

            // the global base address, not the one of this segment
            RemotePointer base_addr;
            size_t offset;
            size_t available_pages;
            /*
             * this map trakcs all pages in current segment
             * if a page is empty, we pop it out from this map
             */
            std::unordered_map<RemotePointer, PageMirror *, RemotePointer::RemotePointerHasher> mirrors;

            Segment(RemotePointer seg, RemotePointer base) : seg(seg), base_addr(base), offset(1) {
                // the first page is not used
                available_pages = Constants::SEGMENT_SIZE / Constants::MEMORY_PAGE_SIZE - 1;
            }

            auto dump() const noexcept -> void;
        };

        struct SegmentTracker {
            Segment *current;

            std::unordered_map<RemotePointer, std::unique_ptr<Segment>, RemotePointer::RemotePointerHasher> segments;

            auto assign_new_seg(RemotePointer seg, RemotePointer base) -> void {
                auto segment = std::make_unique<Segment>(seg, base);
                current = segment.get();
                segments.insert({seg, std::move(segment)});
            }

            auto available(size_t request = 1) -> bool {
                return current->available_pages >= request;
            }

            auto offer_page() -> PageMirror *;
            auto offer_page_group() -> PageGroup *;

            auto dump() const noexcept -> void;

            SegmentTracker() : current(nullptr) {};
            SegmentTracker(const SegmentTracker &) = delete;
            SegmentTracker(SegmentTracker &&) = delete;
            auto operator=(const SegmentTracker &) = delete;
            auto operator=(SegmentTracker &&) = delete;
        };


        // class ComputeNodeAllocator and RemoteMemoryManager are used by
        // class Node::ComputeNode
        class ComputeNodeAllocator {
        public:
            auto apply_for_memory(RemotePointer seg, RemotePointer base) -> void {
                tracker.assign_new_seg(seg, base);
            }

            auto return_memory() -> bool;

            // allocate will NOT fetch a new segment if current segment is used up
            // ComputeNode should fetch
            auto allocate(size_t sz) -> RemotePointer;

            auto free(RemotePointer ptr) -> void;

            auto refill(const std::thread::id &id) -> bool;

            auto refill_single_page(const std::thread::id &id, AllocationClass ac) -> bool;

            auto dump() const noexcept -> void;

            inline auto get_class(size_t sz) -> AllocationClass {
                static AllocationClass table[] = {
                    ChunkUnknown, Chunk16, Chunk32, Chunk64,
                    Chunk128, Chunk256, Chunk512, Chunk1024,
                    Chunk2048, Chunk4096};

                auto ac = (sz - 1) / 16 + 1;
                auto acc = 64 - __builtin_clzll(ac);
                return table[acc];
            }


            ComputeNodeAllocator() = default;
            ComputeNodeAllocator(const ComputeNodeAllocator &) = delete;
            ComputeNodeAllocator(ComputeNodeAllocator &&) = delete;
            auto operator=(const ComputeNodeAllocator &) = delete;
            auto operator=(ComputeNodeAllocator &&) = delete;
        private:
            SegmentTracker tracker;
            std::unordered_map<std::thread::id, PageGroup *> thread_info;
            std::mutex mutex;
        };


        class RemoteMemoryManager {
        public:
            RemoteMemoryManager() = default;

            /*
             * parse config file to find all memory nodes
             * config file format
             * #        tcp              roce            erpc
             * node: 127.0.0.1.1234, 127.0.0.1:4321, 127.0.0.1:3124
             * node: 127.0.0.1.1234, 127.0.0.1:4321, 127.0.0.1:3124
             * node: 127.0.0.1.1234, 127.0.0.1:4321, 127.0.0.1:3124
             */
            auto parse_config_file(const std::string &config) -> bool;

            // connect all memory nodes presented in the config file
            auto connect_memory_nodes() -> bool;

            auto get_base_addr(int node_id) -> RemotePointer;

            auto offer_remote_segment() -> RemotePointer;
            auto recycle_remote_segment(RemotePointer segment) -> bool;


            RemoteMemoryManager(const RemoteMemoryManager &) = delete;
            RemoteMemoryManager(RemoteMemoryManager &&) = delete;
            auto operator=(const RemoteMemoryManager &) = delete;
            auto operator=(RemoteMemoryManager &&) = delete;
        private:
            int current;
            std::vector<std::unique_ptr<Cluster::MemoryNodeInfo>> memory_nodes;
            std::vector<std::unique_ptr<RDMAContext>> rdma;
        };
    }
}
#endif
