#include "fetchkit/fetchkit.hpp"
#include "snapshot_json.h"
#include <array>
#include <chrono>
#include <iostream>
#include <thread>

using namespace fetchkit;
int main(int argc,char** argv) {
    if(argc!=9) return 2;
    const std::string root=argv[1],url=argv[2],key=argv[3],digest=argv[4],mode=argv[5];
    const std::string ns="recovery",version="1",reference="business-reference";
    fetchkit_engine_config config{};fetchkit_engine_config_init(&config);
    config.storage_root=stringView(root);config.max_download_bytes=8ULL*1024*1024*1024;config.allow_http=1;
    config.checkpoint_bytes=std::stoull(argv[6]);config.checkpoint_interval_ms=static_cast<std::uint32_t>(std::stoul(argv[7]));
    config.read_timeout_ms=3000;
    fetchkit_engine engine=0;
    const auto created=fetchkit_engine_create(&config,&engine);
    if(created) { std::cout<<"{\"create_error\":"<<jsonString(fetchkit_status_name(created))<<"}\n";return 0; }
    auto inspect=[&] {
        fetchkit_task_info items[10]{};for(auto& item:items) item.struct_size=sizeof(item);
        std::uint32_t count=0;checkStatus(fetchkit_list_tasks(engine,0,items,10,&count));
        fetchkit_storage_stats stats{};stats.struct_size=sizeof(stats);checkStatus(fetchkit_engine_storage_stats(engine,&stats));
        std::cout<<"{\"records\":[";
        for(std::uint32_t i=0;i<count;++i) { if(i) std::cout<<',';printSnapshot(std::cout,items[i].snapshot); }
        std::cout<<"],\"quarantined\":"<<stats.quarantined_records<<",\"unsupported\":"<<stats.unsupported_records
            <<",\"orphans\":"<<stats.orphan_files<<"}\n";
    };
    try {
        if(mode=="inspect") { inspect();checkStatus(fetchkit_engine_close(engine));return 0; }
        fetchkit_download_request request{};fetchkit_download_request_init(&request);
        request.resource_namespace=stringView(ns);request.resource_key=stringView(key);request.version=stringView(version);
        request.address_reference=stringView(reference);request.url=stringView(url);
        request.expected_sha256=stringView(digest);request.require_digest=std::stoul(argv[8]);
        const std::string name="Authorization",secret="synthetic-secret-must-not-persist";
        const fetchkit_header header{stringView(name),stringView(secret)};request.headers=&header;request.header_count=1;
        fetchkit_task token=0;
        const auto accepted=mode=="attach" ? fetchkit_attach(engine,&request,&token) : fetchkit_enqueue(engine,&request,&token);
        if(accepted) { std::cout<<"{\"enqueue_error\":"<<jsonString(fetchkit_status_name(accepted))<<"}\n";fetchkit_engine_close(engine);return 0; }
        Task task(token);
        auto snapshot=task.query();
        if(mode!="attach") {
            const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(mode.rfind("large",0)==0 ? 600 : 30);
            while(std::chrono::steady_clock::now()<end) {
                snapshot=task.query();
                if(mode=="pause" && snapshot.received_bytes>=16384 && snapshot.state==FETCHKIT_RUNNING) {
                    if(task.pause()==FETCHKIT_OK) { snapshot=task.wait(60000);break; }
                }
                if(mode=="large-stage" && snapshot.received_bytes>=4295000064ULL && snapshot.state==FETCHKIT_RUNNING) {
                    checkStatus(task.pause());snapshot=task.wait(60000);break;
                }
                if(mode=="close" && snapshot.received_bytes>=16384 && snapshot.state==FETCHKIT_RUNNING) {
                    checkStatus(fetchkit_engine_close(engine));engine=0;snapshot=task.query();break;
                }
                if((mode=="cancel-verify" || mode=="pause-verify") && snapshot.state==FETCHKIT_VERIFYING) {
                    checkStatus(mode=="cancel-verify" ? task.cancel() : task.pause());
                    snapshot=task.wait(60000);break;
                }
                if(snapshot.state==FETCHKIT_COMPLETED || snapshot.state==FETCHKIT_FAILED || snapshot.state==FETCHKIT_TASK_CANCELLED) break;
                if(mode=="cancel" && snapshot.received_bytes>=16384) task.cancel();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        fetchkit_lease lease=0;
        const std::string id=snapshot.persistent_id;
        const auto acquired=engine ? fetchkit_resource_acquire(engine,stringView(id),&lease) : FETCHKIT_CLOSED;
        if(engine) checkStatus(fetchkit_engine_close(engine));
        engine=0;
        std::uint64_t leased_bytes=0;
        bool lock_retained=false,lock_released=false,read_ok=true;
        if(lease) {
            fetchkit_engine other=0;
            lock_retained=fetchkit_engine_create(&config,&other)==FETCHKIT_BUSY;
            std::array<std::uint8_t,65536> buffer{};
            for(;;) {
                std::uint32_t count=0;checkStatus(fetchkit_resource_read(lease,leased_bytes,buffer.data(),buffer.size(),&count));
                if(!count) break;
                // Fixture bytes are deliberately stable; read through the public lease.
                for(std::uint32_t i=0;i<count;++i) {
                    const auto position=mode.rfind("large",0)==0 ? (leased_bytes+i)%65536 : leased_bytes+i;
                    if(buffer[i]!=position%251) read_ok=false;
                }
                leased_bytes+=count;
            }
            checkStatus(fetchkit_resource_release(lease));
            lock_released=fetchkit_engine_create(&config,&other)==FETCHKIT_OK;
            if(other) checkStatus(fetchkit_engine_close(other));
        }
        std::cout<<"{\"snapshot\":";printSnapshot(std::cout,snapshot);
        std::cout<<",\"acquire\":"<<jsonString(fetchkit_status_name(acquired))<<",\"leased_bytes\":"<<leased_bytes
            <<",\"lock_retained\":"<<(lock_retained?"true":"false")<<",\"lock_released\":"<<(lock_released?"true":"false")
            <<",\"read_ok\":"<<(read_ok?"true":"false")<<"}\n";
        return 0;
    } catch(const std::exception& e) {
        if(engine) fetchkit_engine_close(engine);
        std::cerr<<e.what()<<'\n';return 3;
    }
}
