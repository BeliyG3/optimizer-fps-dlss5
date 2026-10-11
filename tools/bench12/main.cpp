#include "application.h"
#include "device.h"
#include "options.h"
#include "startup_profile.h"

namespace {
struct ComApartment {
    ComApartment() { Check(CoInitializeEx(nullptr,COINIT_MULTITHREADED),"Initialize COM"); }
    ~ComApartment() { CoUninitialize(); }
};
}
int main(int argc, char **argv)
{
    setvbuf(stdout,nullptr,_IONBF,0);
    StartupMark("process creation to main (loader, ReShade's dxgi.dll)");
    try {
        ComApartment apartment;
        return RunApplication(argc,argv);
    } catch(const MotionSpanError &error) {
        std::fprintf(stderr,"[fail] %s\n",error.what()); return 2;
    } catch(const std::exception &error) {
        std::fprintf(stderr,"[fail] %s\n",error.what()); return 1;
    }
}
