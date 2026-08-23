/*
 *  Copyright 2019-2026 Diligent Graphics LLC
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 *  In no event and under no legal theory, whether in tort (including negligence),
 *  contract, or otherwise, unless required by applicable law (such as deliberate
 *  and grossly negligent acts) or agreed to in writing, shall any Contributor be
 *  liable for any damages, including any direct, indirect, special, incidental,
 *  or consequential damages of any character arising as a result of this License or
 *  out of the use or inability to use the software (including but not limited to damages
 *  for loss of goodwill, work stoppage, computer failure or malfunction, or any and
 *  all other commercial damages or losses), even if such Contributor has been advised
 *  of the possibility of such damages.
 */

#pragma once

// Helpers shared by the OpenXR tests of every backend. The runtime is the one the OpenXR loader finds,
// e.g. Monado with its simulated HMD:
//     set XR_RUNTIME_JSON=C:\monado\install\openxr_monado.json
// The tests are skipped when there is no runtime, no HMD, or no support for the graphics API.

#include <string>

#include "GraphicsTypes.h"
#include "RenderDevice.h"
#include "DeviceContext.h"

#include <openxr/openxr.h>

namespace Diligent
{

namespace Testing
{

/// OpenXR instance with one graphics API extension, and the system of its head-mounted display
class OpenXRTestInstance
{
public:
    OpenXRTestInstance() = default;
    ~OpenXRTestInstance();

    OpenXRTestInstance(const OpenXRTestInstance&) = delete;
    OpenXRTestInstance& operator=(const OpenXRTestInstance&) = delete;

    /// Creates the instance with GraphicsExtension (e.g. XR_KHR_D3D12_ENABLE_EXTENSION_NAME) and finds the HMD.
    /// Returns an empty string on success, otherwise the reason to skip the test.
    std::string Create(const char* GraphicsExtension);

    XrInstance GetInstance() const { return m_Instance; }
    XrSystemId GetSystemId() const { return m_SystemId; }

    /// Attributes for EngineCreateInfo::pXRAttribs
    OpenXRAttribs GetAttribs() const;

    /// Returns the instance function, or null if the runtime does not provide it
    PFN_xrVoidFunction GetProcAddr(const char* Name) const;

private:
    XrInstance m_Instance = XR_NULL_HANDLE;
    XrSystemId m_SystemId = XR_NULL_SYSTEM_ID;
};

/// First queue (Vulkan: queue family) of the adapter whose primary type is exactly Type,
/// or DEFAULT_QUEUE_ID if there is none.
Uint8 FindAdapterQueue(const GraphicsAdapterInfo& Adapter, COMMAND_QUEUE_TYPE Type);

/// Creates a session for pDevice whose graphics binding names pContext, creates a stereo swapchain,
/// and renders and submits FramesToSubmit frames with pContext. Then ends and destroys the session.
/// Reports failures through GTest; check HasFatalFailure() after the call.
void RunOpenXRFrames(const OpenXRTestInstance& Xr,
                     IRenderDevice*            pDevice,
                     IDeviceContext*           pContext,
                     Uint32                    FramesToSubmit);

} // namespace Testing

} // namespace Diligent
