#include "fetchkit/version.h"
#ifdef FETCHKIT_MEASURE_DIGEST
#include "storage/resource_store.h"
#include <cstring>
#endif
#ifdef FETCHKIT_MEASURE_ENGINE
#include "core/engine.h"
namespace {
class MeasureTransport final : public fetchkit::transport::HttpTransport {
public:
    fetchkit::transport::Result execute(const fetchkit::transport::Request&,fetchkit::transport::Sink&,
        fetchkit::transport::Cancellation&) override {return {fetchkit::transport::EndReason::Network,0};}
    fetchkit::transport::UrlInfo resolveUrl(const std::string&,const std::string& url) override {
        return {url,"https://measure.invalid:443",true};
    }
};
void measureEvent(void*,const fetchkit_event*) {}
}
#endif

#if defined(_WIN32)
#define FETCHKIT_MEASURE_EXPORT __declspec(dllexport)
#else
#define FETCHKIT_MEASURE_EXPORT __attribute__((visibility("default")))
#endif

// Measurement-only entry: retain the actual file verifier in a stripped library.
extern "C" FETCHKIT_MEASURE_EXPORT int fetchkit_measure(const char* path,char* digest) noexcept {
#ifdef FETCHKIT_MEASURE_ENGINE
    try {
        if(!path) return 1;
        fetchkit::core::EngineConfig config;config.root=path;config.max_bytes=1048576;config.capacity_bytes=16777216;
        config.staging_retention_ms=86400000;config.max_retries=0;
        auto engine=std::make_shared<fetchkit::core::Engine>(config,std::make_shared<MeasureTransport>());
        fetchkit::core::DownloadRequest request;request.resource_namespace="measure";request.resource_key="object";request.version="1";
        request.request.url="https://measure.invalid/file";
        auto sub=engine->enqueue(request,1);engine->control(sub->task,fetchkit::core::StopReason::Pause);engine->release(sub);engine->close();
        std::uint32_t count=0;fetchkit_lease lease=0;fetchkit_task task=0;fetchkit_engine invalid=0;
        fetchkit_task_snapshot snapshot{};snapshot.struct_size=sizeof(snapshot);
        fetchkit_task_info item{};item.struct_size=sizeof(item);fetchkit_storage_stats stats{};stats.struct_size=sizeof(stats);
        // Keep actual public API code in the measurement without a platform network backend.
        fetchkit_engine_create(nullptr,&invalid);fetchkit_engine_close(0);fetchkit_enqueue(0,nullptr,&task);fetchkit_attach(0,nullptr,&task);
        fetchkit_task_query(0,&snapshot);fetchkit_task_wait(0,0,&snapshot);fetchkit_task_pause(0);fetchkit_task_resume(0);
        fetchkit_task_cancel(0);fetchkit_task_release(0);fetchkit_task_dispatch(0,measureEvent,nullptr,1,&count);
        fetchkit_resource_acquire(0,{},&lease);fetchkit_resource_read(0,0,nullptr,0,&count);fetchkit_resource_release(0);
        fetchkit_resource_pin(0,{},0);fetchkit_resource_set_expiry(0,{},0);fetchkit_resource_remove(0,{});
        fetchkit_storage_trim(0,0);fetchkit_storage_collect_staging(0);fetchkit_list_tasks(0,0,&item,1,&count);fetchkit_engine_storage_stats(0,&stats);
        if(digest) digest[0]=0;return 0;
    }catch(...){return 1;}
#elif defined(FETCHKIT_MEASURE_DIGEST)
    try {
        if(!path || !digest) return 1;
        fetchkit::storage::StagingFile file(std::filesystem::u8path(path),false,true);
        const auto hash=fetchkit::storage::sha256File(file);
        std::memcpy(digest,hash.c_str(),65);return 0;
    }catch(...){return 1;}
#else
    (void)path;(void)digest;return fetchkit::version()[0] ? 0 : 1;
#endif
}
