#include "core/engine_factory.h"
#include "core/download_types.h"
#include <jni.h>
#include <array>
#include <limits>
#include <unordered_map>

namespace {
using namespace fetchkit;
using core::Failure;
JavaVM* java_vm=nullptr;
jclass string_class=nullptr;
jmethodID execute_method=nullptr,resolve_method=nullptr,provide_method=nullptr;

class Environment {
public:
    JNIEnv* env=nullptr;
    Environment() {
        if(java_vm->GetEnv(reinterpret_cast<void**>(&env),JNI_VERSION_1_6)==JNI_EDETACHED) {
            if(java_vm->AttachCurrentThread(&env,nullptr)!=JNI_OK) throw Failure(FETCHKIT_INTERNAL_ERROR);
            attached_=true;
        }
        if(!env || env->PushLocalFrame(64)<0) throw Failure(FETCHKIT_OUT_OF_MEMORY);
    }
    ~Environment() {env->PopLocalFrame(nullptr);if(attached_) java_vm->DetachCurrentThread();}
private:
    bool attached_=false;
};
struct GlobalRef {
    jobject object;
    GlobalRef(JNIEnv* env,jobject value):object(env->NewGlobalRef(value)) {
        if(!object) throw Failure(FETCHKIT_OUT_OF_MEMORY);
    }
    ~GlobalRef() {
        JNIEnv* env=nullptr;bool attached=false;
        if(java_vm->GetEnv(reinterpret_cast<void**>(&env),JNI_VERSION_1_6)==JNI_EDETACHED) {
            if(java_vm->AttachCurrentThread(&env,nullptr)!=JNI_OK) return;
            attached=true;
        }
        if(env) env->DeleteGlobalRef(object); // Also legal while a Java exception is pending.
        if(attached) java_vm->DetachCurrentThread();
    }
};

// JNI modified UTF-8 cannot encode supplementary resource keys correctly.
std::string utf8(JNIEnv* env,jstring value,std::size_t maximum=16384) {
    if(!value) throw Failure(FETCHKIT_INVALID_ARGUMENT);
    const auto length=env->GetStringLength(value);
    if(static_cast<std::size_t>(length)>maximum) throw Failure(FETCHKIT_INVALID_ARGUMENT);
    const auto* input=env->GetStringChars(value,nullptr);
    if(!input) throw Failure(FETCHKIT_OUT_OF_MEMORY);
    std::string result;
    try {
        for(jsize i=0;i<length;++i) {
            std::uint32_t c=input[i];
            if(c>=0xd800 && c<=0xdbff) {
                if(++i>=length || input[i]<0xdc00 || input[i]>0xdfff) throw Failure(FETCHKIT_INVALID_ARGUMENT);
                c=0x10000+((c-0xd800)<<10)+(input[i]-0xdc00);
            } else if(c>=0xdc00 && c<=0xdfff) throw Failure(FETCHKIT_INVALID_ARGUMENT);
            if(!c) throw Failure(FETCHKIT_INVALID_ARGUMENT);
            if(c<0x80) result.push_back(static_cast<char>(c));
            else if(c<0x800) {result.push_back(0xc0|(c>>6));result.push_back(0x80|(c&63));}
            else if(c<0x10000) {result.push_back(0xe0|(c>>12));result.push_back(0x80|((c>>6)&63));result.push_back(0x80|(c&63));}
            else {result.push_back(0xf0|(c>>18));result.push_back(0x80|((c>>12)&63));result.push_back(0x80|((c>>6)&63));result.push_back(0x80|(c&63));}
        }
        if(result.size()>maximum) throw Failure(FETCHKIT_INVALID_ARGUMENT);
    } catch(...) {env->ReleaseStringChars(value,input);throw;}
    env->ReleaseStringChars(value,input);return result;
}
jstring javaString(JNIEnv* env,const std::string& value) {
    std::vector<jchar> result;
    for(std::size_t i=0;i<value.size();) {
        const auto first=static_cast<unsigned char>(value[i++]);std::uint32_t c=first;
        unsigned int count=0;
        if(first>=0xf0) {c=first&7;count=3;} else if(first>=0xe0) {c=first&15;count=2;} else if(first>=0xc0) {c=first&31;count=1;}
        while(count--) {if(i>=value.size()) throw Failure(FETCHKIT_INTERNAL_ERROR);c=(c<<6)|(static_cast<unsigned char>(value[i++])&63);}
        if(c<=0xffff) result.push_back(static_cast<jchar>(c));
        else {c-=0x10000;result.push_back(static_cast<jchar>(0xd800+(c>>10)));result.push_back(static_cast<jchar>(0xdc00+(c&1023)));}
    }
    return env->NewString(result.data(),static_cast<jsize>(result.size()));
}
std::vector<std::string> strings(JNIEnv* env,jobjectArray array,std::size_t limit=512) {
    if(!array || env->GetArrayLength(array)>static_cast<jsize>(limit)) throw Failure(FETCHKIT_INVALID_ARGUMENT);
    std::vector<std::string> result;std::size_t bytes=0;
    for(jsize i=0;i<env->GetArrayLength(array);++i) {
        auto item=static_cast<jstring>(env->GetObjectArrayElement(array,i));
        try {result.push_back(utf8(env,item));} catch(...) {if(item) env->DeleteLocalRef(item);throw;}
        env->DeleteLocalRef(item);bytes+=result.back().size();
        if(bytes>1024*1024) throw Failure(FETCHKIT_INVALID_ARGUMENT);
    }
    return result;
}
jobjectArray javaStrings(JNIEnv* env,const std::vector<std::string>& values) {
    auto result=env->NewObjectArray(static_cast<jsize>(values.size()),string_class,nullptr);
    if(!result) throw Failure(FETCHKIT_OUT_OF_MEMORY);
    for(std::size_t i=0;i<values.size();++i) {
        auto item=javaString(env,values[i]);if(!item) throw Failure(FETCHKIT_OUT_OF_MEMORY);
        env->SetObjectArrayElement(result,static_cast<jsize>(i),item);env->DeleteLocalRef(item);
        if(env->ExceptionCheck()) throw Failure(FETCHKIT_OUT_OF_MEMORY);
    }
    return result;
}
fetchkit_string view(const std::string& value) {return {value.data(),value.size()};}
std::string copy(fetchkit_string value) {return std::string(value.data ? value.data : "",value.size);}
void checked(fetchkit_status status) {if(status) throw Failure(status);}

struct Operation {
    std::mutex mutex;
    transport::Sink* sink;
    transport::Cancellation* cancellation;
    std::exception_ptr failure;
};
std::mutex scope_mutex;
std::uint64_t next_scope=1;
std::unordered_map<std::uint64_t,std::shared_ptr<Operation>> operations;
std::unordered_map<std::uint64_t,fetchkit_request_builder*> provider_scopes;
std::uint64_t allocateScope() {if(next_scope>=INT64_MAX) throw Failure(FETCHKIT_BUSY);return next_scope++;}
std::shared_ptr<Operation> operationFor(jlong token) {
    std::lock_guard<std::mutex> lock(scope_mutex);const auto found=operations.find(token);
    return found==operations.end() ? nullptr : found->second;
}
class OperationScope {
public:
    std::shared_ptr<Operation> operation=std::make_shared<Operation>();
    std::uint64_t token;
    OperationScope(transport::Sink& sink,transport::Cancellation& cancellation) {
        operation->sink=&sink;operation->cancellation=&cancellation;
        std::lock_guard<std::mutex> lock(scope_mutex);token=allocateScope();operations.emplace(token,operation);
    }
    ~OperationScope() {
        {std::lock_guard<std::mutex> lock(scope_mutex);operations.erase(token);}
        std::lock_guard<std::mutex> lock(operation->mutex);operation->sink=nullptr;operation->cancellation=nullptr;
    }
};
class AndroidTransport final : public transport::HttpTransport {
public:
    AndroidTransport(JNIEnv* env,jobject object):java_(std::make_shared<GlobalRef>(env,object)) {}
    transport::Result execute(const transport::Request& request,transport::Sink& sink,transport::Cancellation& cancellation) override {
        Environment scope;auto* env=scope.env;OperationScope operation(sink,cancellation);
        std::vector<std::string> fields;
        for(const auto& header:request.headers) {fields.push_back(header.name);fields.push_back(header.value);}
        auto headers=javaStrings(env,fields);auto url=javaString(env,request.url);
        const auto result=env->CallIntMethod(java_->object,execute_method,static_cast<jlong>(operation.token),url,headers,
            static_cast<jint>(request.connect_timeout_ms),static_cast<jint>(request.read_timeout_ms));
        if(env->ExceptionCheck()) {env->ExceptionClear();return {transport::EndReason::Network,8};}
        if(operation.operation->failure) std::rethrow_exception(operation.operation->failure);
        if(cancellation.isCancelled()) return {transport::EndReason::Cancelled,0};
        if(result<0 || (result&255)>6) return {transport::EndReason::Network,8};
        return {static_cast<transport::EndReason>(result&255),result>>8};
    }
    transport::UrlInfo resolveUrl(const std::string& base,const std::string& location) override {
        Environment scope;auto* env=scope.env;
        auto a=javaString(env,base);auto b=javaString(env,location);
        auto result=static_cast<jobjectArray>(env->CallObjectMethod(java_->object,resolve_method,a,b));
        if(env->ExceptionCheck()) {env->ExceptionClear();throw std::invalid_argument("Invalid HTTP URL");}
        const auto values=strings(env,result,3);
        if(values.size()!=3) throw std::invalid_argument("Invalid HTTP URL");
        return {values[0],values[1],values[2]=="1"};
    }
private:
    std::shared_ptr<GlobalRef> java_;
};
struct Provider {
    std::shared_ptr<GlobalRef> java;
    std::uint64_t engine;
};
std::mutex provider_mutex;
std::unordered_map<std::uint64_t,std::shared_ptr<Provider>> providers;
void releaseProvider(std::uint64_t token) {
    std::shared_ptr<Provider> provider;
    {std::lock_guard<std::mutex> lock(provider_mutex);const auto found=providers.find(token);
     if(found!=providers.end()){provider=std::move(found->second);providers.erase(found);}}
}
struct ReturnedHandle {
    int kind=0;std::uint64_t token=0;
    std::shared_ptr<Provider> provider;
    ~ReturnedHandle() {
        if(!token) return;
        if(kind==1) fetchkit_engine_close(token);
        else if(kind==2) {fetchkit_task_release(token);releaseProvider(token);}
        else if(kind==3) fetchkit_resource_release(token);
    }
};
fetchkit_status provide(void* raw,const fetchkit_request_context* context,fetchkit_request_builder* builder) noexcept {
    std::uint64_t token=0;
    try {
        {std::lock_guard<std::mutex> lock(scope_mutex);token=allocateScope();provider_scopes.emplace(token,builder);}
        struct Cleanup {std::uint64_t token;~Cleanup(){std::lock_guard<std::mutex> lock(scope_mutex);provider_scopes.erase(token);}} cleanup{token};
        Environment scope;auto* env=scope.env;auto* provider=static_cast<Provider*>(raw);
        auto url=javaString(env,copy(context->proposed_url)),origin=javaString(env,copy(context->previous_origin)),reference=javaString(env,copy(context->address_reference));
        auto result=static_cast<jobjectArray>(env->CallObjectMethod(provider->java->object,provide_method,
            static_cast<jlong>(token),static_cast<jint>(context->reason),static_cast<jlong>(context->task_id),
            static_cast<jlong>(context->generation),static_cast<jint>(context->http_status),url,origin,reference));
        if(env->ExceptionCheck()) {env->ExceptionClear();return FETCHKIT_AUTH_REQUIRED;}
        const auto values=strings(env,result,129);
        if(values.empty() || values.size()%2!=1) return FETCHKIT_INVALID_ARGUMENT;
        checked(fetchkit_request_set_url(builder,view(values[0])));
        for(std::size_t i=1;i<values.size();i+=2) checked(fetchkit_request_set_header(builder,view(values[i]),view(values[i+1])));
        return FETCHKIT_OK;
    } catch(const Failure& e) {return e.code;} catch(...) {return FETCHKIT_INTERNAL_ERROR;}
}

void snapshot(std::vector<std::string>& out,const fetchkit_task_snapshot& s) {
    for(auto value:{s.task_id,s.generation,std::uint64_t(s.state),std::uint64_t(s.error),s.received_bytes,s.committed_offset,
        s.total_bytes,std::uint64_t(s.total_known),std::uint64_t(s.integrity),std::uint64_t(s.http_status)}) out.push_back(std::to_string(value));
    out.push_back(std::to_string(s.platform_code));
    for(auto value:{std::uint64_t(s.can_resume),std::uint64_t(s.request_count),std::uint64_t(s.retry_count),std::uint64_t(s.subscriber_count),
        s.next_retry_at_ms,s.expires_at_ms,std::uint64_t(s.available),std::uint64_t(s.pinned)}) out.push_back(std::to_string(value));
    out.emplace_back(s.persistent_id);
}
void event(void* raw,const fetchkit_event* e) {
    auto& out=*static_cast<std::vector<std::string>*>(raw);
    for(auto value:{std::uint64_t(e->kind),e->task_id,e->generation,std::uint64_t(e->state),std::uint64_t(e->error),e->received_bytes,e->committed_offset,
        e->total_bytes,std::uint64_t(e->total_known),std::uint64_t(e->integrity),std::uint64_t(e->http_status),std::uint64_t(e->retry_count),e->next_retry_at_ms}) out.push_back(std::to_string(value));
}

jobjectArray invoke(JNIEnv* env,jobject,jint code,jlong handle,jlongArray raw_numbers,jobjectArray raw_texts,jobject raw_provider,jobject raw_transport) noexcept {
    try {
    std::vector<std::string> output{"0"};
    ReturnedHandle returned;
    try {
        if(!raw_numbers || env->GetArrayLength(raw_numbers)>16) throw Failure(FETCHKIT_INVALID_ARGUMENT);
        std::vector<jlong> n(env->GetArrayLength(raw_numbers));env->GetLongArrayRegion(raw_numbers,0,n.size(),n.data());
        for(auto value:n) if(value<0) throw Failure(FETCHKIT_INVALID_ARGUMENT);
        const auto t=strings(env,raw_texts);
        auto shape=[&](std::size_t numbers,std::size_t texts) {if(n.size()!=numbers || t.size()!=texts) throw Failure(FETCHKIT_INVALID_ARGUMENT);};
        switch(code) {
        case 0: {
            shape(16,1);if(!raw_transport) throw Failure(FETCHKIT_INVALID_ARGUMENT);
            for(std::size_t i=3;i<n.size();++i) if(i!=12 && n[i]>UINT32_MAX) throw Failure(FETCHKIT_INVALID_ARGUMENT);
            fetchkit_engine_config c;fetchkit_engine_config_init(&c);c.storage_root=view(t[0]);
            c.max_download_bytes=n[0];c.storage_capacity_bytes=n[1];c.staging_retention_ms=n[2];c.allow_http=n[3];
            c.max_active_downloads=n[4];c.max_queued_tasks=n[5];c.max_retries=n[6];c.connect_timeout_ms=n[7];
            c.read_timeout_ms=n[8];c.provider_timeout_ms=n[9];c.max_redirects=n[10];c.max_requests=n[11];
            c.checkpoint_bytes=n[12];c.checkpoint_interval_ms=n[13];c.retry_base_delay_ms=n[14];c.retry_max_delay_ms=n[15];
            fetchkit_engine result=0;checked(core::createEngineWithTransport(&c,&result,std::make_shared<AndroidTransport>(env,raw_transport)));
            returned.kind=1;returned.token=result;
            output.push_back(std::to_string(result));break;
        }
        case 1: shape(0,0);checked(fetchkit_engine_close(handle));break;
        case 2: case 3: {
            if(n.size()!=5 || t.size()<7 || t.size()>135 || t.size()%2!=1) throw Failure(FETCHKIT_INVALID_ARGUMENT);
            if(n[0]>1 || n[2]>1 || n[4]>1) throw Failure(FETCHKIT_INVALID_ARGUMENT);
            fetchkit_download_request r;fetchkit_download_request_init(&r);
            r.resource_namespace=view(t[0]);r.resource_key=view(t[1]);r.version=view(t[2]);r.representation_key=view(t[3]);
            r.address_reference=view(t[4]);r.url=view(t[5]);r.expected_sha256=view(t[6]);r.expected_length_known=n[0];
            r.expected_length=n[1];r.require_digest=n[2];r.expires_at_ms=n[3];r.pin_on_create=n[4];
            std::vector<fetchkit_header> fields;
            for(std::size_t i=7;i<t.size();i+=2) fields.push_back({view(t[i]),view(t[i+1])});
            r.headers=fields.data();r.header_count=fields.size();
            std::shared_ptr<Provider> provider;
            if(raw_provider) {provider=std::make_shared<Provider>();provider->java=std::make_shared<GlobalRef>(env,raw_provider);provider->engine=handle;r.provider=provide;r.provider_user_data=provider.get();}
            fetchkit_task result=0;checked(code==2 ? fetchkit_enqueue(handle,&r,&result) : fetchkit_attach(handle,&r,&result));
            returned.kind=2;returned.token=result;
            returned.provider=provider;
            if(provider) {std::lock_guard<std::mutex> lock(provider_mutex);providers.emplace(result,provider);}
            output.push_back(std::to_string(result));
            break;
        }
        case 4: {shape(0,0);fetchkit_task_snapshot s{};s.struct_size=sizeof(s);checked(fetchkit_task_query(handle,&s));snapshot(output,s);break;}
        case 5: shape(0,0);checked(fetchkit_task_pause(handle));break;
        case 6: shape(0,0);checked(fetchkit_task_resume(handle));break;
        case 7: shape(0,0);checked(fetchkit_task_cancel(handle));break;
        case 8: {
            shape(0,0);checked(fetchkit_task_release(handle));releaseProvider(handle);
            break;
        }
        case 9: {
            shape(2,0);if(n[0]>UINT32_MAX || !n[1] || n[1]>100) throw Failure(FETCHKIT_INVALID_ARGUMENT);
            std::vector<fetchkit_task_info> items(n[1]);for(auto& item:items) item.struct_size=sizeof(item);
            std::uint32_t count=0;checked(fetchkit_list_tasks(handle,n[0],items.data(),items.size(),&count));
            for(std::uint32_t i=0;i<count;++i) {const auto& item=items[i];snapshot(output,item.snapshot);
                for(auto value:{item.resource_namespace,item.resource_key,item.version,item.representation_key,item.address_reference}) output.emplace_back(value);}
            break;
        }
        case 10: {
            shape(0,0);fetchkit_storage_stats s{};s.struct_size=sizeof(s);checked(fetchkit_engine_storage_stats(handle,&s));
            for(auto value:{std::uint64_t(s.record_count),std::uint64_t(s.quarantined_records),std::uint64_t(s.unsupported_records),std::uint64_t(s.orphan_files),
                s.capacity_bytes,s.used_bytes,s.reserved_bytes,s.charged_bytes,s.metadata_budget_bytes}) output.push_back(std::to_string(value));break;
        }
        case 11: {shape(0,1);fetchkit_lease lease=0;checked(fetchkit_resource_acquire(handle,view(t[0]),&lease));returned.kind=3;returned.token=lease;output.push_back(std::to_string(lease));break;}
        case 12: shape(1,1);if(n[0]>1) throw Failure(FETCHKIT_INVALID_ARGUMENT);checked(fetchkit_resource_pin(handle,view(t[0]),n[0]));break;
        case 13: shape(1,1);checked(fetchkit_resource_set_expiry(handle,view(t[0]),n[0]));break;
        case 14: shape(0,1);checked(fetchkit_resource_remove(handle,view(t[0])));break;
        case 15: shape(1,0);checked(fetchkit_storage_trim(handle,n[0]));break;
        case 16: shape(0,0);checked(fetchkit_storage_collect_staging(handle));break;
        case 17: {shape(0,0);std::uint32_t count=0;checked(fetchkit_task_dispatch(handle,event,&output,64,&count));break;}
        case 18: shape(0,0);checked(fetchkit_resource_release(handle));break;
        default: throw Failure(FETCHKIT_INVALID_ARGUMENT);
        }
    } catch(const Failure& e) {output={std::to_string(e.code)};}
    catch(const std::bad_alloc&) {output={std::to_string(FETCHKIT_OUT_OF_MEMORY)};}
    catch(...) {output={std::to_string(FETCHKIT_INTERNAL_ERROR)};}
    if(env->ExceptionCheck()) return nullptr;
    try {auto result=javaStrings(env,output);if(output[0]=="0") returned.token=0;return result;} catch(...) {return nullptr;}
    } catch(...) {return nullptr;}
}
jboolean cancelled(JNIEnv*,jobject,jlong token) noexcept {
    try {const auto op=operationFor(token);if(!op) return true;std::lock_guard<std::mutex> lock(op->mutex);return !op->cancellation || op->cancellation->isCancelled();} catch(...) {return true;}
}
jboolean headers(JNIEnv* env,jobject,jlong token,jint status,jobjectArray raw,jstring url) noexcept {
    const auto op=operationFor(token);if(!op) return false;std::lock_guard<std::mutex> lock(op->mutex);
    if(!op->sink || !op->cancellation) return false;
    try {
        const auto fields=strings(env,raw,512);if(fields.size()%2) throw Failure(FETCHKIT_PROTOCOL_MISMATCH);
        transport::Response r;r.status=status;r.effective_url=utf8(env,url);std::size_t bytes=0;
        for(std::size_t i=0;i<fields.size();i+=2) {bytes+=fields[i].size()+fields[i+1].size()+4;if(bytes>65536) throw Failure(FETCHKIT_PROTOCOL_MISMATCH);r.headers.push_back({fields[i],fields[i+1]});}
        return op->sink->onHeaders(r,*op->cancellation);
    } catch(...) {op->failure=std::current_exception();if(env->ExceptionCheck()) env->ExceptionClear();return false;}
}
jboolean data(JNIEnv* env,jobject,jlong token,jbyteArray raw,jint size) noexcept {
    const auto op=operationFor(token);if(!op) return false;std::lock_guard<std::mutex> lock(op->mutex);
    if(!op->sink || !op->cancellation) return false;
    try {
        if(!raw || size<0 || size>65536 || size>env->GetArrayLength(raw)) throw Failure(FETCHKIT_PROTOCOL_MISMATCH);
        std::array<std::uint8_t,65536> bytes{};env->GetByteArrayRegion(raw,0,size,reinterpret_cast<jbyte*>(bytes.data()));
        if(env->ExceptionCheck()) throw Failure(FETCHKIT_INTERNAL_ERROR);
        return op->sink->onData(bytes.data(),size,*op->cancellation);
    } catch(...) {op->failure=std::current_exception();if(env->ExceptionCheck()) env->ExceptionClear();return false;}
}
jboolean providerCancelled(JNIEnv*,jobject,jlong token) noexcept {
    std::lock_guard<std::mutex> lock(scope_mutex);const auto found=provider_scopes.find(token);
    return found==provider_scopes.end() || fetchkit_request_is_cancelled(found->second);
}
jint read(JNIEnv* env,jobject,jlong lease,jlong offset,jbyteArray buffer) noexcept {
    if(!buffer || offset<0) return -FETCHKIT_INVALID_ARGUMENT;
    const auto capacity=env->GetArrayLength(buffer);if(capacity<1 || capacity>65536) return -FETCHKIT_INVALID_ARGUMENT;
    std::array<std::uint8_t,65536> bytes{};std::uint32_t count=0;
    const auto status=fetchkit_resource_read(lease,offset,bytes.data(),capacity,&count);
    if(status) return -static_cast<jint>(status);
    env->SetByteArrayRegion(buffer,0,count,reinterpret_cast<const jbyte*>(bytes.data()));return count;
}
}

bool registerAndroidBridge(JavaVM* vm,JNIEnv* env) noexcept {
    java_vm=vm;
    auto text=env->FindClass("java/lang/String");if(!text) return false;
    string_class=static_cast<jclass>(env->NewGlobalRef(text));env->DeleteLocalRef(text);if(!string_class) return false;
    auto transport=env->FindClass("com/luofelix/fetchkit/AndroidTransport");
    auto provider=env->FindClass("com/luofelix/fetchkit/NativeProvider");
    auto bridge=env->FindClass("com/luofelix/fetchkit/NativeBridge");
    if(!transport || !provider || !bridge) return false;
    execute_method=env->GetMethodID(transport,"execute","(JLjava/lang/String;[Ljava/lang/String;II)I");
    resolve_method=env->GetMethodID(transport,"resolve","(Ljava/lang/String;Ljava/lang/String;)[Ljava/lang/String;");
    provide_method=env->GetMethodID(provider,"provide","(JIJJILjava/lang/String;Ljava/lang/String;Ljava/lang/String;)[Ljava/lang/String;");
    if(!execute_method || !resolve_method || !provide_method) return false;
    const JNINativeMethod methods[]={
        {const_cast<char*>("invoke"),const_cast<char*>("(IJ[J[Ljava/lang/String;Lcom/luofelix/fetchkit/NativeProvider;Lcom/luofelix/fetchkit/AndroidTransport;)[Ljava/lang/String;"),reinterpret_cast<void*>(invoke)},
        {const_cast<char*>("cancelled"),const_cast<char*>("(J)Z"),reinterpret_cast<void*>(cancelled)},
        {const_cast<char*>("headers"),const_cast<char*>("(JI[Ljava/lang/String;Ljava/lang/String;)Z"),reinterpret_cast<void*>(headers)},
        {const_cast<char*>("data"),const_cast<char*>("(J[BI)Z"),reinterpret_cast<void*>(data)},
        {const_cast<char*>("providerCancelled"),const_cast<char*>("(J)Z"),reinterpret_cast<void*>(providerCancelled)},
        {const_cast<char*>("read"),const_cast<char*>("(JJ[B)I"),reinterpret_cast<void*>(read)},
    };
    const auto result=env->RegisterNatives(bridge,methods,sizeof(methods)/sizeof(methods[0]));
    env->DeleteLocalRef(transport);env->DeleteLocalRef(provider);env->DeleteLocalRef(bridge);return result==JNI_OK;
}
