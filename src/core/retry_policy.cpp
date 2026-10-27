#include "core/retry_policy.h"
#include <algorithm>
#include <ctime>
#include <cstdio>
#include <cstring>

namespace fetchkit::core {
bool retryable(fetchkit_status error,std::uint32_t http_status) {
    return error==FETCHKIT_NETWORK || error==FETCHKIT_TIMEOUT ||
        (error==FETCHKIT_HTTP_STATUS && (http_status==408 || http_status==429 || http_status==500 ||
            http_status==502 || http_status==503 || http_status==504));
}

std::uint64_t retryDelay(const EngineConfig& config,std::uint32_t used,std::uint64_t random) {
    const auto base=std::min<std::uint64_t>(config.retry_max_ms,std::uint64_t(config.retry_base_ms)<<std::min(used,16U));
    return std::min<std::uint64_t>(config.retry_max_ms,base+random%(base/4+1));
}

std::uint64_t retryAfter(const std::string& value,std::uint64_t now) {
    if(value.empty() || value.size()>128) return 0;
    if(value.find_first_not_of("0123456789")==std::string::npos) {
        std::uint64_t seconds=0;
        for(char c:value) {
            if(seconds>(static_cast<std::uint64_t>(INT64_MAX)-std::uint64_t(c-'0'))/10) return INT64_MAX;
            seconds=seconds*10+std::uint64_t(c-'0');
        }
        return seconds>(static_cast<std::uint64_t>(INT64_MAX)-now)/1000 ? INT64_MAX : now+seconds*1000;
    }
    const char* formats[]={"%15[^,], %2u %3s %4u %2u:%2u:%2u GMT%n",
        "%15[^,], %2u-%3[^-]-%2u %2u:%2u:%2u GMT%n","%3s %3s %2u %2u:%2u:%2u %4u%n"};
    for(unsigned int index=0;index<3;++index) {
        char weekday[16]{},month[4]{};unsigned int day=0,year_value=0,hour=0,minute=0,second=0;int consumed=-1;
        const auto fields=index==2 ? std::sscanf(value.c_str(),formats[index],weekday,month,&day,&hour,&minute,&second,&year_value,&consumed) :
            std::sscanf(value.c_str(),formats[index],weekday,&day,month,&year_value,&hour,&minute,&second,&consumed);
        if(fields!=7 || consumed!=static_cast<int>(value.size())) continue;
        const char* weekdays[]={"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
        const char* long_weekdays[]={"Sunday","Monday","Tuesday","Wednesday","Thursday","Friday","Saturday"};
        bool valid_weekday=false;
        for(unsigned int d=0;d<7;++d) if(std::strcmp(weekday,index==1 ? long_weekdays[d] : weekdays[d])==0) valid_weekday=true;
        if(!valid_weekday) continue;
        const char* months[]={"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
        int month_index=-1;for(int m=0;m<12;++m) if(std::strcmp(month,months[m])==0) month_index=m;
        int year=static_cast<int>(year_value);
        if(index==1) {
            const auto current=static_cast<std::time_t>(now/1000);
            std::tm today{};
#ifdef _WIN32
            gmtime_s(&today,&current);
#else
            gmtime_r(&current,&today);
#endif
            const auto current_year=today.tm_year+1900;
            year=(current_year/100)*100+static_cast<int>(year_value);
            if(year>current_year+50) year-=100;
        }
        if(year>9999 || month_index<0 || hour>23 || minute>59 || second>60) continue;
        auto leap=[](int y) { return y%4==0 && (y%100!=0 || y%400==0); };
        constexpr int lengths[]={31,28,31,30,31,30,31,31,30,31,30,31};
        if(day<1 || day>static_cast<unsigned int>(lengths[month_index]+(month_index==1 && leap(year)))) continue;
        if(year<1970) return now;
        std::uint64_t days=0;
        for(int y=1970;y<year;++y) days+=leap(y)?366:365;
        for(int m=0;m<month_index;++m) days+=lengths[m]+(m==1 && leap(year));
        days+=day-1;
        const auto due=(days*86400+hour*3600+minute*60+second)*1000;
        return std::max(now,due);
    }
    return 0;
}
}
