#include "ReplicaStore.h"
#include <algorithm>
#include <cstring>
#include <iostream>

namespace {
void put16(ReplicaStore::Block& b, std::size_t o, std::uint16_t n) {
    b[o]=static_cast<char>(n&255); b[o+1]=static_cast<char>((n>>8)&255);
}
void put32(ReplicaStore::Block& b, std::size_t o, std::uint32_t n) {
    for (unsigned i=0;i<4;++i) b[o+i]=static_cast<char>((n>>(8*i))&255);
}
std::uint16_t get16(const ReplicaStore::Block& b, std::size_t o) {
    return static_cast<std::uint8_t>(b[o]) |
      (static_cast<std::uint16_t>(static_cast<std::uint8_t>(b[o+1]))<<8);
}
std::uint32_t get32(const ReplicaStore::Block& b, std::size_t o) {
    std::uint32_t n=0;
    for(unsigned i=0;i<4;++i) n|=static_cast<std::uint32_t>(static_cast<std::uint8_t>(b[o+i]))<<(8*i);
    return n;
}
constexpr std::size_t RECORD_START=32;
constexpr std::size_t RECORD_BYTES=9;
}
ReplicaStore::ReplicaStore(VirtualDisk& a, VirtualDisk& b, VirtualDisk& c):primary_(a),second_(b),third_(c) {}
std::uint32_t ReplicaStore::crc32(const char* data, std::size_t size) {
    std::uint32_t crc=0xFFFFFFFFu;
    for (std::size_t i=0;i<size;++i) {
        crc^=static_cast<std::uint8_t>(data[i]);
        for (unsigned j=0;j<8;++j) crc=(crc>>1)^(0xEDB88320u & (0u-(crc&1u)));
    }
    return ~crc;
}
bool ReplicaStore::emptyDisk(VirtualDisk& disk) {
    Block b{};
    if (!disk.readBlock(0,b)) return false;
    return std::all_of(b.begin(),b.end(),[](char x){return x==0;});
}
bool ReplicaStore::validBlock(std::uint16_t n) const {
    return ready_ && n>=6 && n<VirtualDisk::TOTAL_BLOCKS;
}
ReplicaStore::Block ReplicaStore::encodeCatalog() const {
    Block b{};
    const char magic[8]={'V','F','S','R','E','P','4','!'};
    std::copy(magic,magic+8,b.begin());
    put32(b,8,1); // on-disk catalog version
    put32(b,12,VirtualDisk::TOTAL_BLOCKS);
    for (std::size_t i=6;i<VirtualDisk::TOTAL_BLOCKS;++i) {
        auto o=RECORD_START+i*RECORD_BYTES;
        b[o]=records_[i].valid?1:0;
        put32(b,o+1,records_[i].checksum);
        put16(b,o+5,records_[i].physical2);
        put16(b,o+7,records_[i].physical3);
    }
    put32(b,16,crc32(b.data()+RECORD_START,b.size()-RECORD_START));
    return b;
}
bool ReplicaStore::parseCatalog(const Block& b, std::array<Record,VirtualDisk::TOTAL_BLOCKS>& out) const {
    if (std::string(b.data(),8)!="VFSREP4!" || get32(b,8)!=1 || get32(b,12)!=VirtualDisk::TOTAL_BLOCKS ||
        get32(b,16)!=crc32(b.data()+RECORD_START,b.size()-RECORD_START)) return false;
    for (std::size_t i=6;i<VirtualDisk::TOTAL_BLOCKS;++i) {
        auto o=RECORD_START+i*RECORD_BYTES;
        auto flag=static_cast<std::uint8_t>(b[o]);
        if (flag>1) return false;
        out[i].valid=flag!=0; out[i].checksum=get32(b,o+1);
        out[i].physical2=get16(b,o+5); out[i].physical3=get16(b,o+7);
        if (out[i].valid && (out[i].physical2!=i || out[i].physical3!=i)) return false;
    }
    return true;
}
bool ReplicaStore::saveCatalog() {
    auto b=encodeCatalog();
    // Both catalogs must be written; recovery is not crash-atomic.
    return second_.writeBlock(0,b) && third_.writeBlock(0,b);
}
bool ReplicaStore::loadCatalog() {
    Block a{},b{};
    if (!second_.readBlock(0,a) || !third_.readBlock(0,b)) return false;
    std::array<Record,VirtualDisk::TOTAL_BLOCKS> x{},y{};
    const bool va=parseCatalog(a,x),vb=parseCatalog(b,y);
    // Refuse divergent catalogs, never guess a checksum after partial writes.
    if (!va || !vb || a!=b) return false;
    records_=x;
    return true;
}
bool ReplicaStore::initialize(bool allowCreate) {
    if (ready_) return false;
    // This must be called only AFTER primary is opened/formatted.
    bool made2=false,made3=false;
    if (!second_.open()) {
        if (!allowCreate || !second_.create() || !second_.open()) return false;
        made2=true;
    }
    if (!third_.open()) {
        if (!allowCreate || !third_.create() || !third_.open()) return false;
        made3=true;
    }
    ready_=true;
    if (made2 && made3 && emptyDisk(second_) && emptyDisk(third_)) {
        fresh_=true;
        if (saveCatalog()) return true;
    } else if (!made2 && !made3 && loadCatalog()) return true;
    ready_=false;
    return false;
}
bool ReplicaStore::hasRecord(std::uint16_t n) const { return validBlock(n) && records_[n].valid; }
bool ReplicaStore::write(std::uint16_t n, const Block& b) {
    if (!validBlock(n)) return false;
    // Write all replicas before changing the active inode pointer (FileSystem handles inode COW).
    if (!second_.writeBlock(n,b) || !third_.writeBlock(n,b) || !primary_.writeBlock(n,b)) return false;
    const auto old=records_[n];
    records_[n]={crc32(b.data(),b.size()),n,n,true};
    if (saveCatalog()) return true;
    records_[n]=old;
    return false;
}
bool ReplicaStore::seed(std::uint16_t n, const Block& b) {
    if (!validBlock(n) || records_[n].valid) return false;
    return write(n,b);
}
bool ReplicaStore::read(std::uint16_t n, Block& out) {
    lastRepairs_=0;
    if (!hasRecord(n)) return false;
    VirtualDisk* disks[3]={&primary_,&second_,&third_};
    Block blocks[3]{};
    bool good[3]{};
    int donor=-1;
    for (unsigned i=0;i<3;++i) {
        const std::uint16_t physical=i==0?n:(i==1?records_[n].physical2:records_[n].physical3);
        good[i]=disks[i]->readBlock(physical,blocks[i]) &&
           crc32(blocks[i].data(),blocks[i].size())==records_[n].checksum;
        if (good[i] && donor==-1) donor=static_cast<int>(i);
    }
    if (donor<0) { std::cerr<<"[CRC32] Block "<<n<<": no valid replica.\n"; return false; }
    out=blocks[donor];
    for (unsigned i=0;i<3;++i) if (!good[i]) {
        const std::uint16_t physical=i==0?n:(i==1?records_[n].physical2:records_[n].physical3);
        if (disks[i]->writeBlock(physical,out)) {
            ++lastRepairs_;
            std::cout<<"[Recovery] Repaired disk"<<(i+1)<<" block "<<n<<" using disk"<<(donor+1)<<".\n";
        } else std::cerr<<"[Recovery] Could not repair disk"<<(i+1)<<" block "<<n<<".\n";
    }
    return true;
}
bool ReplicaStore::corrupt(unsigned diskNumber, std::uint16_t n) {
    if (!hasRecord(n) || diskNumber<1 || diskNumber>3) return false;
    VirtualDisk* d=diskNumber==1?&primary_:(diskNumber==2?&second_:&third_);
    std::uint16_t physical=diskNumber==1?n:(diskNumber==2?records_[n].physical2:records_[n].physical3);
    Block b{};
    if (!d->readBlock(physical,b)) return false;
    b[0]^=static_cast<char>(0x5A);
    return d->writeBlock(physical,b);
}
