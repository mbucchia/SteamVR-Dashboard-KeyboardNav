// MIT License
//
// Copyright(c) 2025 Matthieu Bucchianeri
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include "pch.h"

#include "ShimDriverManager.h"
#include "DetourUtils.h"
#include "Tracing.h"

namespace {
    using namespace driver_shim;

    DEFINE_DETOUR_FUNCTION(bool,
                           IVRServerDriverHost_TrackedDeviceAdded,
                           vr::IVRServerDriverHost* driverHost,
                           const char* pchDeviceSerialNumber,
                           vr::ETrackedDeviceClass eDeviceClass,
                           vr::ITrackedDeviceServerDriver* pDriver) {
        TraceLocalActivity(local);
        TraceLoggingWriteStart(local,
                               "IVRServerDriverHost_TrackedDeviceAdded",
                               TLArg(pchDeviceSerialNumber, "DeviceSerialNumber"),
                               TLArg((int)eDeviceClass, "DeviceClass"));

        vr::ITrackedDeviceServerDriver* shimmedDriver = pDriver;

        // Only shim the desired device class and if they are registered by the target driver.
        if (IsTargetDriver(_ReturnAddress())) {
            TraceLoggingWriteTagged(local, "IVRServerDriverHost_TrackedDeviceAdded", TLArg(true, "IsTargetDriver"));
            if (eDeviceClass == vr::TrackedDeviceClass_HMD) {
                DriverLog("Shimming new TrackedDeviceClass_HMD with HmdShimDriver");
                shimmedDriver = CreateHmdShimDriver(pDriver);
            }
        }

        const auto status = original_IVRServerDriverHost_TrackedDeviceAdded(
            driverHost, pchDeviceSerialNumber, eDeviceClass, shimmedDriver);

        TraceLoggingWriteStop(local, "IVRServerDriverHost_TrackedDeviceAdded", TLArg(status, "Status"));

        return status;
    }

} // namespace

namespace driver_shim {

    void InstallShimDriverHook() {
        TraceLocalActivity(local);
        TraceLoggingWriteStart(local, "InstallShimDriverHook");

        DriverLog("Installing IVRServerDriverHost::TrackedDeviceAdded hook");

        // TODO: Consider hooking all flavors, though 006 is the most common it appears.
        vr::EVRInitError eError;
        DetourMethodAttach(vr::VRDriverContext()->GetGenericInterface("IVRServerDriverHost_006", &eError),
                           0 /* TrackedDeviceAdded() */,
                           hooked_IVRServerDriverHost_TrackedDeviceAdded,
                           original_IVRServerDriverHost_TrackedDeviceAdded);

        TraceLoggingWriteStop(local, "InstallShimDriverHook");
    }

    bool IsTargetDriver(void* returnAddress) {
#if 0
        HMODULE callerModule;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)returnAddress,
                               &callerModule)) {
            return callerModule == GetModuleHandleA("driver_oculus.dll");
        }
        return false;
#else
        // Any driver.
        return true;
#endif
    }

} // namespace driver_shim
