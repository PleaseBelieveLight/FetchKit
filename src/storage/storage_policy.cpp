#include "storage/resource_store.h"
#include "storage/fault.h"
#include <algorithm>

namespace fetchkit::storage {
void ResourceStore::requireOpen() const {if(closed_) throw core::Failure(FETCHKIT_CLOSED);}
void ResourceStore::shutdown() {std::lock_guard<std::recursive_mutex> lock(mutex_);closed_=true;}
bool ResourceStore::protectedId(const std::string& id) const {
    std::lock_guard<std::mutex> lock(protection_mutex_);
    const auto lease=leases_.find(id);
    return protected_ids_.count(id) || deleting_ids_.count(id) || (lease!=leases_.end() && lease->second);
}

void ResourceStore::protect(const std::string& id,bool require_unleased,bool require_existing) {
    std::lock_guard<std::mutex> lock(protection_mutex_);
    if(require_existing && !present_ids_.count(id)) throw core::Failure(FETCHKIT_NOT_FOUND);
    const auto lease=leases_.find(id);
    if(deleting_ids_.count(id) || (require_unleased && lease!=leases_.end() && lease->second)) throw core::Failure(FETCHKIT_BUSY);
    protected_ids_.insert(id);
}
void ResourceStore::unprotect(const std::string& id) {
    std::lock_guard<std::mutex> lock(protection_mutex_);protected_ids_.erase(id);
}

std::uint64_t ResourceStore::charged() const {
    const auto metadata=slots_.size()*metadataBudget();
    if(unowned_bytes_>UINT64_MAX-metadata) return UINT64_MAX;
    std::uint64_t result=unowned_bytes_+metadata;
    for(const auto& allocation:allocations_) {
        if(allocation.second>UINT64_MAX-result) return UINT64_MAX;
        result+=allocation.second;
    }
    return result;
}

void ResourceStore::recount() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    slots_.clear();data_bytes_.clear();allocations_.clear();record_bytes_.clear();unowned_bytes_=0;
    for(const auto& pair:records_) { slots_.insert(pair.first);data_bytes_[pair.first]=0; }
    for(const auto folder:{"tmp","objects","records","quarantine"}) {
        for(const auto& entry:std::filesystem::directory_iterator(directory_.root()/folder)) {
            if(!std::filesystem::is_regular_file(entry.symlink_status())) continue;
            const auto bytes=entry.file_size();const auto id=entry.path().stem().u8string();
            const bool owned=slots_.count(id)!=0;
            if(owned && entry.path().extension()==".rec" && folder==std::string("records")) record_bytes_[id]+=bytes;
            else if(owned && ((entry.path().extension()==".part" && folder==std::string("tmp")) ||
                (entry.path().extension()==".obj" && folder==std::string("objects")))) data_bytes_[id]+=bytes;
            else unowned_bytes_=bytes>UINT64_MAX-unowned_bytes_ ? UINT64_MAX : unowned_bytes_+bytes;
        }
    }
    allocations_=data_bytes_;
}

void ResourceStore::erase(const std::string& id,bool own_task) {
    requireOpen();
    const auto found=records_.find(id);
    if(found==records_.end()) throw core::Failure(FETCHKIT_NOT_FOUND);
    if(found->second.pinned) throw core::Failure(FETCHKIT_BUSY);
    {
        std::lock_guard<std::mutex> lock(protection_mutex_);
        const auto lease=leases_.find(id);
        if(deleting_ids_.count(id) || (lease!=leases_.end() && lease->second) || (!own_task && protected_ids_.count(id))) throw core::Failure(FETCHKIT_BUSY);
        deleting_ids_.insert(id);
    }
    try {
        auto tombstone=found->second;
        tombstone.state=FETCHKIT_REMOVED;tombstone.committed=0;tombstone.total=tombstone.request.expected_length;
        tombstone.etag.clear();tombstone.integrity=FETCHKIT_INTEGRITY_NONE;tombstone.error=FETCHKIT_NOT_FOUND;
        save(tombstone);
        faultPoint("remove_commit_after");
        removeFile(stagingPath(id));removeFile(objectPath(id));
        removeFile(directory_.root()/"records"/(id+".next"));
        removeFile(directory_.root()/"records"/(id+".rec"));
        records_.erase(id);slots_.erase(id);allocations_.erase(id);data_bytes_.erase(id);record_bytes_.erase(id);
    } catch(...) {
        std::lock_guard<std::mutex> lock(protection_mutex_);deleting_ids_.erase(id);throw;
    }
    std::lock_guard<std::mutex> lock(protection_mutex_);deleting_ids_.erase(id);
}

bool ResourceStore::evictOne(const std::string& exclude) {
    const auto now=clock_->nowMs();
    std::optional<Record> candidate;
    for(const auto& item:records_) {
        const auto& r=item.second;
        if(r.id==exclude || r.pinned || protectedId(r.id)) continue;
        const bool stale=r.state!=FETCHKIT_COMPLETED && r.state!=FETCHKIT_PUBLISHING &&
            now>=r.updated_at_ms && now-r.updated_at_ms>=retention_ms_;
        if(r.state!=FETCHKIT_COMPLETED && !stale && r.state!=FETCHKIT_REMOVED) continue;
        const bool expired=r.expires_at_ms && r.expires_at_ms<=now;
        const bool old_expired=candidate && candidate->expires_at_ms && candidate->expires_at_ms<=now;
        if(!candidate || (expired && !old_expired) || (expired==old_expired && r.access_order<candidate->access_order)) candidate=r;
    }
    if(!candidate) return false;
    try { erase(candidate->id);return true; }
    catch(const core::Failure& error) { if(error.code==FETCHKIT_BUSY) return false;throw; }
}

void ResourceStore::ensureCapacity(const std::string& exclude) {
    while(charged()>capacity_) if(!evictOne(exclude)) throw core::Failure(FETCHKIT_QUOTA_EXCEEDED);
}

void ResourceStore::reserve(const std::string& id,std::uint64_t offset,std::optional<std::uint64_t> total) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    requireOpen();
    if(!records_.count(id)) throw core::Failure(FETCHKIT_NOT_FOUND);
    const auto old=allocations_[id];
    constexpr std::uint64_t block=65536;
    if(offset>INT64_MAX || (total && *total>INT64_MAX)) throw core::Failure(FETCHKIT_QUOTA_EXCEEDED);
    const auto requested=total ? std::max(offset,*total) : ((offset+block-1)/block)*block;
    allocations_[id]=std::max(data_bytes_[id],requested);
    try { ensureCapacity(id); }
    catch(...) { allocations_[id]=old;throw; }
}

void ResourceStore::observeSize(const std::string& id,std::uint64_t size) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if(!slots_.count(id)) return;
    data_bytes_[id]=size;allocations_[id]=std::max(allocations_[id],size);
}
void ResourceStore::endAttempt(const std::string& id) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const auto size=data_bytes_.find(id);
    if(size!=data_bytes_.end()) allocations_[id]=size->second;
}

Record ResourceStore::recreate(const Record& previous) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    requireOpen();
    if(records_.count(previous.id)) erase(previous.id,true);
    auto r=create(previous.request);r.generation=previous.generation;
    r.expires_at_ms=previous.expires_at_ms;save(r,true);return r;
}

std::shared_ptr<ResourceLease> ResourceStore::acquire(const std::string& id) {
    Record r;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        requireOpen();
        const auto found=records_.find(id);
        if(found==records_.end() || found->second.state!=FETCHKIT_COMPLETED ||
            (found->second.expires_at_ms && found->second.expires_at_ms<=clock_->nowMs())) throw core::Failure(FETCHKIT_NOT_FOUND);
        r=found->second;
        std::lock_guard<std::mutex> protection(protection_mutex_);
        if(protected_ids_.count(id) || deleting_ids_.count(id)) throw core::Failure(FETCHKIT_BUSY);
        if(lease_count_>=1024) throw core::Failure(FETCHKIT_BUSY);
        ++leases_[id];++lease_count_;
    }
    std::shared_ptr<ResourceLease> lease;
    try {
        lease=std::make_shared<ResourceLease>(shared_from_this(),r);
        checkObject(r);
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        auto current=records_.at(id);
        if(access_order_==UINT64_MAX-1) throw core::Failure(FETCHKIT_BUSY);
        current.access_order=++access_order_;save(current,true);
        return lease;
    } catch(...) {
        if(!lease) releaseLease(id);
        throw;
    }
}
void ResourceStore::releaseLease(const std::string& id) noexcept {
    std::lock_guard<std::mutex> lock(protection_mutex_);
    const auto found=leases_.find(id);
    if(found!=leases_.end() && found->second) {
        --lease_count_;if(--found->second==0) leases_.erase(found);
    }
}

void ResourceStore::pin(const std::string& id,bool value) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    requireOpen();
    const auto found=records_.find(id);if(found==records_.end()) throw core::Failure(FETCHKIT_NOT_FOUND);
    auto r=found->second;r.pinned=value;save(r,true);
}
void ResourceStore::setExpiry(const std::string& id,std::uint64_t expires) {
    if(expires>INT64_MAX) throw core::Failure(FETCHKIT_INVALID_ARGUMENT);
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    requireOpen();
    const auto found=records_.find(id);if(found==records_.end()) throw core::Failure(FETCHKIT_NOT_FOUND);
    auto r=found->second;r.expires_at_ms=expires;save(r,true);
}
void ResourceStore::remove(const std::string& id) { std::lock_guard<std::recursive_mutex> lock(mutex_);requireOpen();erase(id); }
void ResourceStore::trimTo(std::uint64_t target) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    requireOpen();
    while(charged()>target) if(!evictOne()) throw core::Failure(FETCHKIT_QUOTA_EXCEEDED);
}
void ResourceStore::collectStaging() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    requireOpen();
    const auto now=clock_->nowMs();std::vector<std::string> stale;
    for(const auto& item:records_) {
        const auto& r=item.second;
        if(r.state!=FETCHKIT_COMPLETED && r.state!=FETCHKIT_PUBLISHING && !r.pinned && !protectedId(r.id) &&
            now>=r.updated_at_ms && now-r.updated_at_ms>=retention_ms_) stale.push_back(r.id);
    }
    for(const auto& id:stale) {
        try { erase(id); } catch(const core::Failure& error) { if(error.code!=FETCHKIT_BUSY) throw; }
    }
}
bool ResourceStore::available(const std::string& id) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const auto found=records_.find(id);
    return found!=records_.end() && found->second.state==FETCHKIT_COMPLETED &&
        (!found->second.expires_at_ms || found->second.expires_at_ms>clock_->nowMs());
}
}
