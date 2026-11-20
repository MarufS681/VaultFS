#ifndef VAULTFS_REPLICA_STORE_H
#define VAULTFS_REPLICA_STORE_H
#include "VirtualDisk.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

// Phase 4: data-only three-copy replication. Primary filesystem metadata
// remains in disk1. Replica catalog lives in block 0 of disk2 and disk3.
class ReplicaStore {
public:
    using Block = std::array<char, VirtualDisk::BLOCK_SIZE>;
    ReplicaStore(VirtualDisk& primary, VirtualDisk& second, VirtualDisk& third);
    bool initialize(bool allowCreate);
    bool isFresh() const { return fresh_; }
    bool hasRecord(std::uint16_t logical) const;
    bool seed(std::uint16_t logical, const Block& bytes);
    bool write(std::uint16_t logical, const Block& bytes);
    bool read(std::uint16_t logical, Block& bytes);
    bool corrupt(unsigned diskNumber, std::uint16_t logical);
    unsigned lastRepairCount() const { return lastRepairs_; }
private:
    struct Record {
        std::uint32_t checksum = 0;
        std::uint16_t physical2 = 0;
        std::uint16_t physical3 = 0;
        bool valid = false;
    };
    VirtualDisk& primary_;
    VirtualDisk& second_;
    VirtualDisk& third_;
    std::array<Record, VirtualDisk::TOTAL_BLOCKS> records_{};
    bool ready_ = false;
    bool fresh_ = false;
    unsigned lastRepairs_ = 0;
    bool saveCatalog();
    bool loadCatalog();
    bool parseCatalog(const Block& bytes, std::array<Record, VirtualDisk::TOTAL_BLOCKS>& result) const;
    Block encodeCatalog() const;
    static std::uint32_t crc32(const char* data, std::size_t count);
    static bool emptyDisk(VirtualDisk& disk);
    bool validBlock(std::uint16_t logical) const;
};
#endif
