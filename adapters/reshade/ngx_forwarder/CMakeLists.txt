add_library(peripheral_warp_ngx_forwarder MODULE ngx_forwarder.cpp)
target_compile_features(peripheral_warp_ngx_forwarder PRIVATE cxx_std_20)
target_link_libraries(peripheral_warp_ngx_forwarder PRIVATE kernel32)
# The file name must contain "nvngx.dll": nvngx_dlssnr.dll accepts calls only from such modules.
set_target_properties(peripheral_warp_ngx_forwarder PROPERTIES
    PREFIX ""
    OUTPUT_NAME "nvngx.dll_optimizerfps"
    SUFFIX ".dll")
if(MSVC)
    target_compile_options(peripheral_warp_ngx_forwarder PRIVATE /W4 /WX /EHsc /permissive-)
endif()
