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

#include "SharedMemory.h"

namespace {
    using namespace driver_shim;

    // The HmdShimDriver driver wraps another ITrackedDeviceServerDriver instance with the intent to override
    // properties and behaviors.
    struct HmdShimDriver : public vr::ITrackedDeviceServerDriver {
        HmdShimDriver(vr::ITrackedDeviceServerDriver* shimmedDevice) : m_shimmedDevice(shimmedDevice) {
            TraceLocalActivity(local);
            TraceLoggingWriteStart(local, "HmdShimDriver_Ctor");
            TraceLoggingWriteStop(local, "HmdShimDriver_Ctor");
        }

        vr::EVRInitError Activate(uint32_t unObjectId) override {
            TraceLocalActivity(local);
            TraceLoggingWriteStart(local, "HmdShimDriver_Activate", TLArg(unObjectId, "ObjectId"));

            // Activate the real device driver.
            const auto status = m_shimmedDevice->Activate(unObjectId);

            m_deviceIndex = unObjectId;

            const vr::PropertyContainerHandle_t container =
                vr::VRProperties()->TrackedDeviceToPropertyContainer(m_deviceIndex);

            vr::VRProperties()->SetStringProperty(
                container, vr::Prop_InputProfilePath_String, "{keyboard_nav}/input/keyboard_nav_hmd_profile.json");

            vr::VRDriverInput()->CreateBooleanComponent(container, "/input/system/click", &m_clickComponent);

            *m_sharedFileHandle.put() = CreateFileMapping(
                INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(*m_sharedMemory), L"KeyboardNav.SharedMemory");
            if (m_sharedFileHandle) {
                m_sharedMemory = (shared::SharedMemory*)MapViewOfFile(
                    m_sharedFileHandle.get(), FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(*m_sharedMemory));
                memset(m_sharedMemory, 0, sizeof(*m_sharedMemory));
            }

            m_active = true;
            m_updateThread = std::thread(&HmdShimDriver::BackgroundThread, this);

            TraceLoggingWriteStop(local, "HmdShimDriver_Activate");

            return status;
        }

        void Deactivate() override {
            TraceLocalActivity(local);
            TraceLoggingWriteStart(local, "HmdShimDriver_Deactivate", TLArg(m_deviceIndex, "ObjectId"));

            if (m_active.exchange(false) && m_updateThread.joinable()) {
                m_updateThread.join();
            }

            if (m_sharedMemory) {
                UnmapViewOfFile(m_sharedMemory);
                m_sharedMemory = nullptr;
            }
            if (m_sharedFileHandle) {
                m_sharedFileHandle.reset();
            }

            m_deviceIndex = vr::k_unTrackedDeviceIndexInvalid;

            m_shimmedDevice->Deactivate();

            DriverLog("Deactivated device shimmed with HmdShimDriver");

            TraceLoggingWriteStop(local, "HmdShimDriver_Deactivate");
        }

        void EnterStandby() override {
            m_shimmedDevice->EnterStandby();
        }

        void* GetComponent(const char* pchComponentNameAndVersion) override {
            return m_shimmedDevice->GetComponent(pchComponentNameAndVersion);
        }

        vr::DriverPose_t GetPose() override {
            return m_shimmedDevice->GetPose();
        }

        void DebugRequest(const char* pchRequest, char* pchResponseBuffer, uint32_t unResponseBufferSize) override {
            m_shimmedDevice->DebugRequest(pchRequest, pchResponseBuffer, unResponseBufferSize);
        }

        void BackgroundThread() {
            TraceLocalActivity(local);
            TraceLoggingWriteStart(local, "HmdShimDriver_UpdateThread");

            DriverLog("Hello from HmdShimDriver::UpdateThread");
            SetThreadDescription(GetCurrentThread(), L"HmdShimDriver_UpdateThread");

            while (true) {
                // Wait for the next time to update.
                {
                    TraceLocalActivity(sleep);
                    TraceLoggingWriteStart(sleep, "HmdShimDriver_UpdateThread_Sleep");

                    std::this_thread::sleep_for(std::chrono::milliseconds(5));

                    TraceLoggingWriteStop(sleep, "HmdShimDriver_UpdateThread_Sleep", TLArg(m_active.load(), "Active"));

                    if (!m_active) {
                        break;
                    }
                }

                // See if the client process is already started.
                if (m_clientProcessInfo.dwProcessId) {
                    if (!WaitForSingleObject(m_clientProcessInfo.hProcess, 0)) {
                        CloseHandle(m_clientProcessInfo.hProcess);

                        // Mark as finished.
                        m_clientProcessInfo = {};
                    }
                }

                // Start the shell app if needed.
                if (!m_clientProcessInfo.dwProcessId) {
                    HMODULE thisDll = nullptr;
                    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                       (LPCWSTR)&CreateHmdShimDriver,
                                       &thisDll);
                    WCHAR path[MAX_PATH]{};
                    GetModuleFileNameW(thisDll, path, MAX_PATH);
                    const auto root = std::filesystem::path(path).parent_path();

                    STARTUPINFO info = {sizeof(info)};
                    if (!CreateProcess((root / L"client_utility.exe").wstring().c_str(),
                                       nullptr,
                                       nullptr,
                                       nullptr,
                                       FALSE,
                                       0,
                                       nullptr,
                                       root.parent_path().parent_path().c_str(),
                                       &info,
                                       &m_clientProcessInfo)) {
                        DriverLog("Failed to start client utility: %d", GetLastError());
                    }
                    CloseHandle(m_clientProcessInfo.hThread);
                }

                if (InterlockedExchange(&m_sharedMemory->sendClickEvent, 0)) {
                    vr::VRDriverInput()->UpdateBooleanComponent(m_clickComponent, true, 0);
                    m_inClickEvent = true;
                } else if (m_inClickEvent) {
                    vr::VRDriverInput()->UpdateBooleanComponent(m_clickComponent, false, 0);
                    m_inClickEvent = false;
                }
            }

            DriverLog("Bye from HmdShimDriver::UpdateThread");

            TraceLoggingWriteStop(local, "HmdShimDriver_UpdateThread");
        }

        vr::ITrackedDeviceServerDriver* const m_shimmedDevice;
        vr::TrackedDeviceIndex_t m_deviceIndex = vr::k_unTrackedDeviceIndexInvalid;
        vr::VRInputComponentHandle_t m_clickComponent = vr::k_ulInvalidInputComponentHandle;
        wil::unique_handle m_sharedFileHandle;
        shared::SharedMemory* m_sharedMemory = nullptr;
        std::atomic<bool> m_active = false;
        std::thread m_updateThread;
        PROCESS_INFORMATION m_clientProcessInfo = {};
        bool m_inClickEvent = false;
    };
} // namespace

namespace driver_shim {
    vr::ITrackedDeviceServerDriver* CreateHmdShimDriver(vr::ITrackedDeviceServerDriver* shimmedDriver) {
        return new HmdShimDriver(shimmedDriver);
    }

} // namespace driver_shim
