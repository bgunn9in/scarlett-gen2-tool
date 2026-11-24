#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <setupapi.h>
#include <bcrypt.h>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

namespace {
struct Error {const char* step;DWORD code;};
[[noreturn]] void fail(const char* step,DWORD code=ERROR_INVALID_DATA) {throw Error{step,code};}
void require(bool condition,const char* step) {if(!condition)fail(step);}
struct Handle {
    HANDLE value=INVALID_HANDLE_VALUE;
    explicit Handle(HANDLE h=INVALID_HANDLE_VALUE):value(h) {}
    ~Handle() {if(value!=INVALID_HANDLE_VALUE && value)CloseHandle(value);}
    Handle(const Handle&)=delete;Handle& operator=(const Handle&)=delete;
};
struct Info {
    HDEVINFO value;
    ~Info() {if(value!=INVALID_HANDLE_VALUE)SetupDiDestroyDeviceInfoList(value);}
};
struct Hash {
    BCRYPT_ALG_HANDLE algorithm=nullptr;BCRYPT_HASH_HANDLE hash=nullptr;
    ~Hash() {if(hash)BCryptDestroyHash(hash);if(algorithm)BCryptCloseAlgorithmProvider(algorithm,0);}
};
struct Request {std::uint64_t token;std::uint16_t family,operation;std::uint32_t reserved;};
struct Response {std::uint32_t status,bytes;};
static_assert(sizeof(void*)==8 && sizeof(Request)==16 && offsetof(Request,family)==8 && sizeof(Response)==8);
constexpr GUID pal_id{0xac4d0455,0x50d7,0x4498,{0xb3,0xcd,0x9a,0x41,0xd1,0x30,0xb7,0x59}};
constexpr DWORD ioctl=0x222008;
enum class Query {root,device_descriptor};
struct Specification {std::uint16_t family,operation;DWORD input,output;};
constexpr Specification spec(Query q) {
    switch(q) {
    case Query::root:return {0x400,4,16,256};
    case Query::device_descriptor:return {0x401,0x17,20,26}; // ONLY DWORD subselector1.
    }
    return {}; // Invalid enum cannot reach DeviceIoControl.
}
template<class T> T get(const void* bytes,std::size_t offset) {
    T value{};std::memcpy(&value,static_cast<const BYTE*>(bytes)+offset,sizeof(value));return value;
}
void check_hash(HANDLE file,const char* expected) {
    LARGE_INTEGER size{};require(GetFileSizeEx(file,&size)!=FALSE,"driver-size");
    require(size.QuadPart>0 && size.QuadPart<=64*1024*1024,"driver-size-bound");
    std::vector<BYTE> object;Hash h;require(BCryptOpenAlgorithmProvider(&h.algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)==0,"sha-open");
    ULONG bytes=0,object_bytes=0;
    require(BCryptGetProperty(h.algorithm,BCRYPT_OBJECT_LENGTH,reinterpret_cast<PUCHAR>(&object_bytes),sizeof(object_bytes),&bytes,0)==0 && bytes==sizeof(object_bytes) && object_bytes<=4096,"sha-object-size");
    object.resize(object_bytes);
    require(BCryptCreateHash(h.algorithm,&h.hash,object.data(),object_bytes,nullptr,0,0)==0,"sha-create");
    std::array<BYTE,4096> block{};LONGLONG total=0;
    for(;;) {
        DWORD got=0;if(!ReadFile(file,block.data(),DWORD(block.size()),&got,nullptr))fail("driver-read",GetLastError());
        if(!got)break;total+=got;require(total<=size.QuadPart,"driver-read-size");
        require(BCryptHashData(h.hash,block.data(),got,0)==0,"sha-update");
    }
    require(total==size.QuadPart,"driver-read-total");std::array<BYTE,32> result{};
    require(BCryptFinishHash(h.hash,result.data(),ULONG(result.size()),0)==0,"sha-finish");
    constexpr char hex[]="0123456789abcdef";char actual[65]{};
    for(unsigned i=0;i<32;++i) {actual[2*i]=hex[result[i]>>4];actual[2*i+1]=hex[result[i]&15];}
    require(std::strcmp(actual,expected)==0,"installed-driver-sha-mismatch");
}
std::wstring system_driver(const wchar_t* name) {
    wchar_t directory[MAX_PATH]{};const auto n=GetSystemDirectoryW(directory,MAX_PATH);
    require(n>0 && n<MAX_PATH,"system-directory");return std::wstring(directory)+L"\\drivers\\"+name;
}
std::wstring pal_path() {
    Info set{SetupDiGetClassDevsW(&pal_id,nullptr,nullptr,DIGCF_PRESENT|DIGCF_DEVICEINTERFACE)};
    if(set.value==INVALID_HANDLE_VALUE)fail("SetupDi-class",GetLastError());
    std::wstring found;unsigned matches=0;
    for(DWORD i=0;i<16;++i) {
        SP_DEVICE_INTERFACE_DATA interface_data{};interface_data.cbSize=sizeof(interface_data);
        if(!SetupDiEnumDeviceInterfaces(set.value,nullptr,&pal_id,i,&interface_data)) {
            if(GetLastError()==ERROR_NO_MORE_ITEMS)break;fail("SetupDi-enum",GetLastError());
        }
        if(i==15)fail("SetupDi-interface-limit");
        DWORD needed=0;SetupDiGetDeviceInterfaceDetailW(set.value,&interface_data,nullptr,0,&needed,nullptr);
        require(GetLastError()==ERROR_INSUFFICIENT_BUFFER && needed>=sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W) && needed<=4096,"SetupDi-detail-bound");
        std::vector<BYTE> storage(needed);auto* detail=reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(storage.data());detail->cbSize=sizeof(*detail);
        SP_DEVINFO_DATA device{};device.cbSize=sizeof(device);
        if(!SetupDiGetDeviceInterfaceDetailW(set.value,&interface_data,detail,needed,nullptr,&device))fail("SetupDi-detail",GetLastError());
        const auto capacity=(needed-offsetof(SP_DEVICE_INTERFACE_DETAIL_DATA_W,DevicePath))/sizeof(wchar_t);
        std::size_t length=0;while(length<capacity && detail->DevicePath[length])++length;
        require(length<capacity,"SetupDi-path-terminator");
        if(length<4 || _wcsicmp(detail->DevicePath+length-4,L"\\pal"))continue;
        wchar_t service[64]{};DWORD type=0,service_bytes=0;
        if(!SetupDiGetDeviceRegistryPropertyW(set.value,&device,SPDRP_SERVICE,&type,reinterpret_cast<BYTE*>(service),sizeof(service),&service_bytes))fail("SetupDi-service",GetLastError());
        require(type==REG_SZ && service_bytes>=2 && service_bytes<=sizeof(service) && service_bytes%2==0 && service[service_bytes/2-1]==0 && !_wcsicmp(service,L"FocusriteUsbSwRoot"),"unexpected-PAL-service");
        found.assign(detail->DevicePath,length);++matches;
    }
    require(matches==1,"ambiguous-or-missing-PAL-interface");return found;
}
using Buffer=std::array<BYTE,256>;
Buffer query(HANDLE device,Query kind,std::uint64_t token) {
    const auto s=spec(kind);require(s.input && s.output<=256 && ((kind==Query::root)==(token==0)),"allowlist");
    std::array<BYTE,20> input{};const Request header{token,s.family,s.operation,0};std::memcpy(input.data(),&header,sizeof(header));
    if(kind==Query::device_descriptor) {const DWORD subselector=1;std::memcpy(input.data()+16,&subselector,4);}
    Buffer output{};Handle event(CreateEventW(nullptr,TRUE,FALSE,nullptr));if(!event.value)fail("request-event",GetLastError());
    OVERLAPPED pending{};pending.hEvent=event.value;DWORD returned=0;
    const auto immediate=DeviceIoControl(device,ioctl,input.data(),s.input,output.data(),s.output,&returned,&pending);
    if(!immediate) {
        const auto error=GetLastError();if(error!=ERROR_IO_PENDING)fail("cached-ioctl",error);
        const auto waited=WaitForSingleObject(event.value,2000);
        if(waited!=WAIT_OBJECT_0) {
            // Cancellation is a local IRP action. Never release OVERLAPPED or
            // buffers before actual completion; the final drain may still wait.
            const auto cancel=CancelIoEx(device,&pending);const auto cancel_error=cancel?ERROR_SUCCESS:GetLastError();
            DWORD ignored=0;(void)GetOverlappedResult(device,&pending,&ignored,TRUE);
            fail("cached-ioctl-deadline",cancel_error==ERROR_SUCCESS || cancel_error==ERROR_NOT_FOUND?ERROR_TIMEOUT:cancel_error);
        }
    }
    if(!GetOverlappedResult(device,&pending,&returned,TRUE))fail("cached-completion",GetLastError());
    require(returned==s.output,"cached-response-capacity");const auto response=get<Response>(output.data(),0);
    require(response.status==0 && response.bytes<=s.output-sizeof(Response),"cached-response-status");
    if(kind!=Query::root)require(response.bytes==s.output-sizeof(Response),"cached-response-length");
    return output;
}
std::uint64_t selected(const Buffer& b) {
    // Source400/4 has no payload length and writes96B even when its old check
    // says88. Our256B capacity also covers ten source token slots through127.
    require(get<std::uint32_t>(b.data(),88)==1,"require-single-selected-device");
    const auto token=get<std::uint64_t>(b.data(),8);require(token && token==get<std::uint64_t>(b.data(),48),"selected-device-list-mismatch");return token;
}
void descriptor(const Buffer& b) {
    require(b[8]==18 && b[9]==1 && get<std::uint16_t>(b.data(),16)==0x1235 && get<std::uint16_t>(b.data(),18)==0x8202,"not-Scarlett-2i2-Gen2");
}
}
