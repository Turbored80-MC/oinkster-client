#include "Oinkster/ControlPipe.hpp"
#include "Oinkster/PipeWait.hpp"
#include <Windows.h>
#include <sddl.h>
#include <fstream>
#include <string>
#include <string_view>
#include <iterator>
#include <stdexcept>

namespace Oinkster {
namespace { constexpr std::wstring_view PipeName=LR"(\\.\pipe\LOCAL\Oinkster.Client.Control.v1)"; }

ControlPipe::~ControlPipe(){Stop();}

bool ControlPipe::Start(){
    if(worker_.joinable())return false;
    stopping_.store(false);
    stopEvent_=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    if(!stopEvent_)return false;
    worker_=std::thread([this]{Run();});
    return true;
}

void ControlPipe::Stop() noexcept {
    stopping_.store(true);
    if(stopEvent_)SetEvent(stopEvent_);
    if(worker_.joinable())worker_.join();
    if(stopEvent_){CloseHandle(stopEvent_);stopEvent_=nullptr;}
}

void ControlPipe::Run() noexcept {
    PSECURITY_DESCRIPTOR descriptor{};
    if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GRGW;;;OW)(A;;GRGW;;;AC)(A;;GA;;;SY)",SDDL_REVISION_1,&descriptor,nullptr))return;
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES),descriptor,FALSE};
    while(!stopping_.load()){
        HANDLE pipe=CreateNamedPipeW(PipeName.data(),PIPE_ACCESS_DUPLEX|FILE_FLAG_FIRST_PIPE_INSTANCE|FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_MESSAGE|PIPE_READMODE_MESSAGE|PIPE_WAIT|PIPE_REJECT_REMOTE_CLIENTS,1,65536,65536,1000,&attributes);
        if(pipe==INVALID_HANDLE_VALUE)break;
        const auto connected=PipeWait::AwaitConnection(pipe,stopEvent_);
        if(connected==PipeWait::Result::Completed&&!stopping_.load()){
            std::string request(65536,'\0');DWORD read{};
            // Bounded read: a client that connects without sending must not
            // be able to wedge shutdown.
            const bool received=PipeWait::Read(pipe,request.data(),static_cast<DWORD>(request.size()),read,
                stopEvent_,2000)==PipeWait::Result::Completed;
            request.resize(received?read:0);
            std::string reply="error";
            if(request=="ping")reply="ok";
            else if(request=="get"){
                std::ifstream input(settingsPath_,std::ios::binary);reply.assign(std::istreambuf_iterator<char>(input),{});
                if(reply.empty())reply="{}";
            } else if(request.starts_with("set\n")) {
                try {
                    std::filesystem::create_directories(settingsPath_.parent_path());auto temporary=settingsPath_;temporary+=L".incoming";
                    {std::ofstream output(temporary,std::ios::binary|std::ios::trunc);output.write(request.data()+4,static_cast<std::streamsize>(request.size()-4));output.flush();if(!output.good())throw std::runtime_error("write");}
                    // Atomic replace - see SettingsStore::Save. The old
                    // remove()-then-rename() had already deleted the real
                    // settings file by the time a failing rename was noticed.
                    if(!MoveFileExW(temporary.c_str(),settingsPath_.c_str(),
                            MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)){
                        std::error_code ignored;std::filesystem::remove(temporary,ignored);
                        throw std::runtime_error("replace");
                    }
                    reloadRequested_.store(true);reply="ok";
                } catch(...) {reply="error";}
            }
            (void)PipeWait::Write(pipe,reply.data(),static_cast<DWORD>(reply.size()),stopEvent_,2000);
            FlushFileBuffers(pipe);DisconnectNamedPipe(pipe);
        }
        CloseHandle(pipe);
        if(connected==PipeWait::Result::Stopped)break;
    }
    LocalFree(descriptor);
}
}
