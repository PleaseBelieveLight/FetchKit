#include "storage/resource_store.h"
#include "storage/fault.h"
#include <algorithm>

namespace fetchkit::storage {
namespace {
std::vector<std::uint8_t> readRecord(const std::filesystem::path& path) {
    if (!regularFile(path)) throw core::Failure(FETCHKIT_INTEGRITY_MISMATCH);
    StagingFile file(path,false,true);
    const auto size=file.size();
    if (size>65556 || size<20) throw core::Failure(FETCHKIT_INTEGRITY_MISMATCH);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    std::size_t at=0;
    while (at<bytes.size()) {
        const auto count=file.read(bytes.data()+at,bytes.size()-at);
        if (!count) throw core::Failure(FETCHKIT_INTEGRITY_MISMATCH);
        at+=count;
    }
    return bytes;
}
bool missing(const std::filesystem::path& path) { return !regularFile(path); }
bool leafExists(const std::filesystem::path& path) {
    std::error_code error;
    const auto status=std::filesystem::symlink_status(path,error);
    if(error==std::errc::no_such_file_or_directory) return false;
    if(error) throw core::Failure(FETCHKIT_IO_ERROR,error.value());
    return std::filesystem::exists(status);
}
}

ResourceStore::ResourceStore(const std::string& root) : directory_(root) { scan(); }
std::filesystem::path ResourceStore::stagingPath(const std::string& id) const {
    if (!validId(id)) throw core::Failure(FETCHKIT_INVALID_ARGUMENT);
    return directory_.root()/"tmp"/(id+".part");
}
std::filesystem::path ResourceStore::objectPath(const std::string& id) const {
    if (!validId(id)) throw core::Failure(FETCHKIT_INVALID_ARGUMENT);
    return directory_.root()/"objects"/(id+".obj");
}
std::optional<Record> ResourceStore::find(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found=records_.find(id);
    return found==records_.end() ? std::optional<Record>{} : found->second;
}
std::vector<Record> ResourceStore::list(std::uint32_t offset, std::uint32_t capacity) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Record> result;
    auto it=records_.begin();
    while (offset-- && it!=records_.end()) ++it;
    while (capacity-- && it!=records_.end()) { result.push_back(it->second); ++it; }
    return result;
}
fetchkit_storage_stats ResourceStore::stats() {
    std::lock_guard<std::mutex> lock(mutex_);
    return {sizeof(fetchkit_storage_stats),static_cast<std::uint32_t>(records_.size()),quarantined_,unsupported_,orphans_,0};
}

void ResourceStore::save(const Record& record) {
    const auto bytes=encodeRecord(record);
    const auto path=directory_.root()/"records"/(record.id+".rec");
    const auto next=directory_.root()/"records"/(record.id+".next");
    std::lock_guard<std::mutex> lock(mutex_);
    // A failed prior commit may leave our own temporary record. Never trust it.
    removeFile(next);
    {
        StagingFile file(next,true);
        faultPoint("record_write_before");
        file.write(bytes.data(),12);
        faultPoint("record_partial_write_after");
        file.write(bytes.data()+12,bytes.size()-12);
        faultPoint("record_sync_before");
        file.sync();
        faultPoint("record_sync_after");
    }
    faultPoint("record_replace_before");
    atomicRename(next,path,true);
    faultPoint("record_replace_after");
    faultPoint("record_directory_sync_before");
    syncDirectory(path.parent_path());
    faultPoint("record_directory_sync_after");
    auto persisted=record;
    persisted.request.request={}; persisted.request.provider={};
    records_[record.id]=std::move(persisted);
}

Record ResourceStore::create(const core::DownloadRequest& request) {
    Record r; r.id=resourceId(request); r.request=request; r.total=request.expected_length;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (records_.size()>=1024) throw core::Failure(FETCHKIT_BUSY);
        if (blocked_ids_.count(r.id)) throw core::Failure(FETCHKIT_BUSY);
    }
    // An unrecognized/orphan file is never adopted or silently overwritten.
    if (!missing(stagingPath(r.id)) || !missing(objectPath(r.id))) throw core::Failure(FETCHKIT_BUSY);
    {
        StagingFile file(stagingPath(r.id),true); file.sync();
    }
    syncDirectory(stagingPath(r.id).parent_path());
    save(r);
    return r;
}

void ResourceStore::discard(Record& r) {
    r.state=FETCHKIT_TASK_CANCELLED; r.committed=0; r.total=r.request.expected_length;
    r.etag.clear(); r.integrity=FETCHKIT_INTEGRITY_NONE; r.error=FETCHKIT_CANCELLED;
    save(r); // Tombstone first: a killed cleanup cannot resurrect data.
    faultPoint("cancel_commit_after");
    removeFile(stagingPath(r.id));
}

void ResourceStore::publish(Record& r, transport::Cancellation& cancellation, const std::function<void()>& barrier) {
    r.state=FETCHKIT_VERIFYING; save(r);
    faultPoint("verify_before");
    r.integrity=verifyFile(stagingPath(r.id),r,&cancellation);
    faultPoint("verify_after");
    barrier(); // Serialize the final pause/cancel decision before Publishing.
    r.total=r.committed; r.state=FETCHKIT_PUBLISHING; save(r);
    faultPoint("publishing_commit_after");
    faultPoint("object_rename_before");
    atomicRename(stagingPath(r.id),objectPath(r.id),false);
    faultPoint("object_rename_after");
    faultPoint("object_directory_sync_before");
    syncDirectory(stagingPath(r.id).parent_path());
    syncDirectory(objectPath(r.id).parent_path());
    faultPoint("object_directory_sync_after");
    r.state=FETCHKIT_COMPLETED; r.error=FETCHKIT_OK; save(r);
    faultPoint("completed_commit_after");
}

void ResourceStore::checkObject(const Record& r) {
    if (r.state!=FETCHKIT_COMPLETED) throw core::Failure(FETCHKIT_NOT_FOUND);
    verifyFile(objectPath(r.id),r);
}

void ResourceStore::quarantine(const std::filesystem::path& path) {
    for (unsigned int n=0;n<100000;++n) {
        const auto target=directory_.root()/"quarantine"/(path.filename().u8string()+"."+std::to_string(n));
        if (!std::filesystem::exists(std::filesystem::symlink_status(target))) {
            atomicRename(path,target,false);
            syncDirectory(path.parent_path());syncDirectory(target.parent_path());return;
        }
    }
    throw core::Failure(FETCHKIT_BUSY);
}

void ResourceStore::repair(Record& r) {
    const auto part=stagingPath(r.id), object=objectPath(r.id);
    if (r.state==FETCHKIT_COMPLETED || r.state==FETCHKIT_PUBLISHING) {
        try {
            const auto candidate=leafExists(object) ? object : r.state==FETCHKIT_PUBLISHING ? part : object;
            r.integrity=verifyFile(candidate,r);
            if (candidate==part) {
                atomicRename(part,object,false);
                syncDirectory(part.parent_path());syncDirectory(object.parent_path());
            }
            r.state=FETCHKIT_COMPLETED; r.error=FETCHKIT_OK; save(r);
            return;
        } catch (const core::Failure& error) {
            if (error.code!=FETCHKIT_INTEGRITY_MISMATCH && error.code!=FETCHKIT_PERMISSION_DENIED) throw;
            r.state=FETCHKIT_FAILED; r.error=FETCHKIT_INTEGRITY_MISMATCH;
            r.integrity=FETCHKIT_INTEGRITY_NONE; r.committed=0; r.total=r.request.expected_length; r.etag.clear();
            save(r);
            if (leafExists(object)) quarantine(object);
        }
    }
    if(leafExists(part)) {
        try { regularFile(part); }
        catch(const core::Failure& error) {
            if(error.code!=FETCHKIT_PERMISSION_DENIED) throw;
            // Invalidate the record before isolating a suspicious leaf; never
            // follow its target or infer a resumable prefix from it.
            r.state=FETCHKIT_FAILED;r.error=FETCHKIT_INTEGRITY_MISMATCH;
            r.committed=0;r.etag.clear();r.total=r.request.expected_length;r.integrity=FETCHKIT_INTEGRITY_NONE;
            save(r);quarantine(part);
        }
    }
    if (r.state==FETCHKIT_TASK_CANCELLED) { removeFile(part); return; }
    if (missing(part)) {
        r.committed=0; r.etag.clear(); r.total=r.request.expected_length;
        save(r);
        StagingFile file(part,true); file.sync(); syncDirectory(part.parent_path());
    } else {
        StagingFile file(part,false);
        if (file.size()<r.committed) {
            r.committed=0; r.etag.clear(); r.total=r.request.expected_length;
            save(r); // Invalidate before truncation/overwriting.
        }
        if (file.size()!=r.committed) { file.truncate(r.committed); file.sync(); }
    }
    if (r.state!=FETCHKIT_FAILED) { r.state=FETCHKIT_PAUSED; r.error=FETCHKIT_OK; }
    r.integrity=FETCHKIT_INTEGRITY_NONE;
    save(r);
}

void ResourceStore::scan() {
    const auto root=directory_.root();
    for(const auto& entry:std::filesystem::directory_iterator(root/"quarantine")) {
        const auto name=entry.path().filename().u8string();
        if(name.size()>69 && name.substr(64,5)==".rec." && validId(name.substr(0,64))) {
            if(blocked_ids_.size()>=4096) throw core::Failure(FETCHKIT_BUSY);
            blocked_ids_.insert(name.substr(0,64));
        }
    }
    std::vector<std::filesystem::path> paths;
    for (const auto& entry:std::filesystem::directory_iterator(root/"records")) {
        if (paths.size()>=4096) throw core::Failure(FETCHKIT_BUSY);
        paths.push_back(entry.path());
    }
    std::sort(paths.begin(),paths.end());
    for (const auto& path:paths) {
        if(path.extension()==".next" && validId(path.stem().u8string())) {
            quarantine(path);++orphans_;continue;
        }
        if (path.extension()!=".rec" || !validId(path.stem().u8string())) { ++orphans_; continue; }
        std::optional<Record> decoded;
        try {
            decoded=decodeRecord(readRecord(path));
            if (decoded->id!=path.stem().u8string()) throw core::Failure(FETCHKIT_INTEGRITY_MISMATCH);
        } catch (const core::Failure& error) {
            if (error.code!=FETCHKIT_INTEGRITY_MISMATCH && error.code!=FETCHKIT_UNSUPPORTED_METADATA && error.code!=FETCHKIT_PERMISSION_DENIED) throw;
            if (error.code==FETCHKIT_UNSUPPORTED_METADATA) ++unsupported_;
            blocked_ids_.insert(path.stem().u8string());
            // Moving a symlink itself does not follow its target.
            quarantine(path); ++quarantined_; continue;
        }
        if (records_.size()>=1024) throw core::Failure(FETCHKIT_BUSY);
        repair(*decoded);
        if (!find(decoded->id)) save(*decoded);
    }
    for (const auto name:{"tmp","objects"}) {
        for (const auto& entry:std::filesystem::directory_iterator(root/name)) {
            const auto id=entry.path().stem().u8string();
            const auto found=find(id);
            const bool owned=found && ((name==std::string("tmp") && entry.path().extension()==".part" && found->state!=FETCHKIT_COMPLETED && found->state!=FETCHKIT_TASK_CANCELLED) ||
                (name==std::string("objects") && entry.path().extension()==".obj" && found->state==FETCHKIT_COMPLETED));
            if (!owned) ++orphans_;
        }
    }
}

ResourceLease::ResourceLease(std::shared_ptr<ResourceStore> store,const Record& r)
    : store_(std::move(store)),file_(store_->objectPath(r.id),false,true) {
    if (file_.size()!=r.committed) throw core::Failure(FETCHKIT_INTEGRITY_MISMATCH);
}
std::uint32_t ResourceLease::read(std::uint64_t offset,std::uint8_t* buffer,std::uint32_t capacity) {
    std::lock_guard<std::mutex> lock(mutex_);
    file_.seek(offset);
    return static_cast<std::uint32_t>(file_.read(buffer,capacity));
}
}
