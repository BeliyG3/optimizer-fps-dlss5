#include "application.h"
#include "device.h"

namespace {
struct ComApartment {
    ComApartment() { Check(CoInitializeEx(nullptr,COINIT_MULTITHREADED),"Initialize COM"); }
    ~ComApartment() { CoUninitialize(); }
};
}
int main(int argc, char **argv)
{
    setvbuf(stdout,nullptr,_IONBF,0);
    try {
        ComApartment apartment;
        return RunApplication(argc,argv);
    } catch(const std::exception &error) {
        std::fprintf(stderr,"[fail] %s\n",error.what()); return 1;
    }
}
