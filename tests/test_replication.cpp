#include "FileSystem.h"
#include "ReplicaStore.h"
#include <cassert>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>

int main() {
    namespace fsys=std::filesystem;
    const auto dir=fsys::temp_directory_path()/
        ("vaultfs_p4_"+std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count()));
    fsys::create_directories(dir);
    auto p1=(dir/"disk1.bin").string();
    auto p2=(dir/"disk2.bin").string();
    auto p3=(dir/"disk3.bin").string();
    const std::string content(18000,'Q'); // 5 data blocks, exercises single-indirect pointers
    {
        VirtualDisk a(p1),b(p2),c(p3);
        assert(a.create() && a.open());
        ReplicaStore store(a,b,c);
        FileSystem files(a,&store);
        assert(files.format());
        assert(store.initialize(true));
        assert(store.isFresh());
        assert(files.bootstrapReplicas());
        assert(files.createFile("test.txt"));
        assert(files.writeFile("test.txt",content));
        std::string result;
        assert(files.readFile("test.txt",result) && result==content);
        assert(files.corruptFileBlock(1,"test.txt",0));
        assert(files.readFile("test.txt",result) && result==content);
        assert(store.lastRepairCount()==0); // final block is clean
        assert(files.corruptFileBlock(2,"test.txt",4));
        assert(files.corruptFileBlock(3,"test.txt",4));
        assert(files.readFile("test.txt",result) && result==content);
        assert(files.deleteFile("test.txt"));
        assert(files.createFile("persistent.txt"));
        assert(files.writeFile("persistent.txt","stored across restarts"));
    }
    {
        VirtualDisk a(p1),b(p2),c(p3);
        assert(a.open());
        ReplicaStore store(a,b,c);
        FileSystem files(a,&store);
        assert(files.mount() && store.initialize(true) && !store.isFresh());
        std::string result;
        assert(files.readFile("persistent.txt",result) && result=="stored across restarts");
        assert(files.corruptFileBlock(1,"persistent.txt",0));
        assert(files.corruptFileBlock(2,"persistent.txt",0));
        assert(files.readFile("persistent.txt",result) && result=="stored across restarts");
        assert(store.lastRepairCount()==2);
    }
    fsys::remove_all(dir);
    std::cout<<"Phase 4 replication, recovery, indirect addressing and restart: PASSED\n";
}
