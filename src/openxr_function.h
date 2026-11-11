#pragma once
#include <openxr/openxr.h>
#include <cstring>
#include <stdexcept>
#include <string>

namespace olvr {
inline void xr_check(XrResult result,const char* operation) {
    if (XR_FAILED(result))
        throw std::runtime_error(std::string(operation)+" XrResult="+std::to_string(result));
}

template<class T>
void xr_load_function(PFN_xrGetInstanceProcAddr resolve,XrInstance instance,
                      const char* name,T& function) {
    if (!resolve) throw std::invalid_argument("A real OpenXR resolver is required");
    PFN_xrVoidFunction address=nullptr;
    xr_check(resolve(instance,name,&address),name);
    if (!address) throw std::runtime_error(std::string("Missing OpenXR entry: ")+name);
    static_assert(sizeof(address)==sizeof(function));
    std::memcpy(&function,&address,sizeof(function));
}
}
