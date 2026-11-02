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

// Runs an OpenXR session on a Direct3D12 device created on the adapter that the runtime requires
// (XR_KHR_D3D12_enable). See OpenXRTestHelpers.hpp for how the runtime is found.

#include "WinHPreface.h"
#include <d3d12.h>
#include "WinHPostface.h"

#include "GPUTestingEnvironment.hpp"
#include "OpenXRTestHelpers.hpp"

#include "RenderDeviceD3D12.h"
#include "EngineFactoryD3D12.h"
#include "CommandQueueD3D12.h"
#include "OpenXRUtilities.h"

#define XR_USE_GRAPHICS_API_D3D12
#include <openxr/openxr_platform.h>

#include <iostream>
#include <vector>

#include "gtest/gtest.h"

using namespace Diligent;
using namespace Diligent::Testing;

namespace
{

TEST(OpenXRD3D12, SessionWithGraphicsContextNotFirst)
{
    GPUTestingEnvironment* pEnv = GPUTestingEnvironment::GetInstance();
    if (pEnv->GetDevice()->GetDeviceInfo().Type != RENDER_DEVICE_TYPE_D3D12)
        GTEST_SKIP() << "Direct3D12 only";

    GPUTestingEnvironment::ScopedReset EnvironmentAutoReset;

    OpenXRTestInstance Xr;
    {
        const std::string SkipReason = Xr.Create(XR_KHR_D3D12_ENABLE_EXTENSION_NAME);
        if (!SkipReason.empty())
            GTEST_SKIP() << SkipReason;
    }

    // The adapter and the minimum feature level that the runtime requires
    XrGraphicsRequirementsD3D12KHR Requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR};
    {
        PFN_xrGetD3D12GraphicsRequirementsKHR xrGetD3D12GraphicsRequirementsKHR =
            reinterpret_cast<PFN_xrGetD3D12GraphicsRequirementsKHR>(Xr.GetProcAddr("xrGetD3D12GraphicsRequirementsKHR"));
        ASSERT_NE(xrGetD3D12GraphicsRequirementsKHR, nullptr);
        ASSERT_EQ(xrGetD3D12GraphicsRequirementsKHR(Xr.GetInstance(), Xr.GetSystemId(), &Requirements), XR_SUCCESS);
    }

    RefCntAutoPtr<IEngineFactoryD3D12> pFactory{pEnv->GetDevice()->GetEngineFactory(), IID_EngineFactoryD3D12};
    ASSERT_NE(pFactory, nullptr);

    Uint32 NumAdapters = 0;
    pFactory->EnumerateAdapters(Version{11, 0}, NumAdapters, nullptr);
    std::vector<GraphicsAdapterInfo> Adapters(NumAdapters);
    if (NumAdapters > 0)
        pFactory->EnumerateAdapters(Version{11, 0}, NumAdapters, Adapters.data());
    ASSERT_GT(NumAdapters, 0u);

    // Two immediate contexts: a compute context first, so that the graphics context has ContextId 1.
    // The binding must name the graphics context's queue, and the swapchain images must be usable in that context.
    const Uint8 ComputeQueue  = FindAdapterQueue(Adapters[0], COMMAND_QUEUE_TYPE_COMPUTE);
    const Uint8 GraphicsQueue = FindAdapterQueue(Adapters[0], COMMAND_QUEUE_TYPE_GRAPHICS);
    ASSERT_NE(ComputeQueue, DEFAULT_QUEUE_ID);
    ASSERT_NE(GraphicsQueue, DEFAULT_QUEUE_ID);

    ImmediateContextCreateInfo ContextCI[] = {
        {"XR compute", ComputeQueue},
        {"XR graphics", GraphicsQueue},
    };

    const OpenXRAttribs XRAttribs = Xr.GetAttribs();

    EngineD3D12CreateInfo EngineCI;
    EngineCI.pXRAttribs            = &XRAttribs;
    EngineCI.NumImmediateContexts  = _countof(ContextCI);
    EngineCI.pImmediateContextInfo = ContextCI;

    RefCntAutoPtr<IRenderDevice>  pDevice;
    RefCntAutoPtr<IDeviceContext> pContexts[2];
    {
        IDeviceContext* ppContexts[2] = {};
        pFactory->CreateDeviceAndContextsD3D12(EngineCI, &pDevice, ppContexts);
        pContexts[0].Attach(ppContexts[0]);
        pContexts[1].Attach(ppContexts[1]);
    }
    ASSERT_NE(pDevice, nullptr);
    ASSERT_NE(pContexts[0], nullptr);
    ASSERT_NE(pContexts[1], nullptr);
    IDeviceContext* pGraphicsCtx = pContexts[1];

    RefCntAutoPtr<IRenderDeviceD3D12> pDeviceD3D12{pDevice, IID_RenderDeviceD3D12};
    ASSERT_NE(pDeviceD3D12, nullptr);
    ID3D12Device* pd3d12Device = pDeviceD3D12->GetD3D12Device();

    // The device is on the runtime's adapter
    {
        const LUID DeviceLuid = pd3d12Device->GetAdapterLuid();
        std::cout << "    Device selected by the runtime: " << pDevice->GetAdapterInfo().Description
                  << ", LUID " << std::hex << DeviceLuid.HighPart << ':' << DeviceLuid.LowPart
                  << " (runtime LUID " << Requirements.adapterLuid.HighPart << ':' << Requirements.adapterLuid.LowPart << ")" << std::dec << std::endl;
        EXPECT_EQ(DeviceLuid.LowPart, Requirements.adapterLuid.LowPart);
        EXPECT_EQ(DeviceLuid.HighPart, Requirements.adapterLuid.HighPart);
    }

    // The graphics binding names the device and the ID3D12CommandQueue of the graphics context
    {
        RefCntAutoPtr<IDataBlob> pBinding;
        GetOpenXRGraphicsBinding(pDevice, pGraphicsCtx, &pBinding);
        ASSERT_NE(pBinding, nullptr);
        const XrGraphicsBindingD3D12KHR& Binding = *pBinding->GetConstDataPtr<XrGraphicsBindingD3D12KHR>();

        ID3D12CommandQueue* pd3d12CtxQueue = nullptr;
        {
            RefCntAutoPtr<ICommandQueueD3D12> pQueueD3D12{pGraphicsCtx->LockCommandQueue(), IID_CommandQueueD3D12};
            pd3d12CtxQueue = pQueueD3D12 ? pQueueD3D12->GetD3D12CommandQueue() : nullptr;
            pQueueD3D12.Release();
            pGraphicsCtx->UnlockCommandQueue();
        }
        ASSERT_NE(pd3d12CtxQueue, nullptr);

        std::cout << "    Graphics context: ContextId " << Uint32{pGraphicsCtx->GetDesc().ContextId}
                  << ", binding queue is the context's queue: " << (Binding.queue == pd3d12CtxQueue ? "yes" : "no") << std::endl;
        EXPECT_EQ(Binding.type, XR_TYPE_GRAPHICS_BINDING_D3D12_KHR);
        EXPECT_EQ(Binding.device, pd3d12Device);
        EXPECT_EQ(Binding.queue, pd3d12CtxQueue);
        // OpenXR composites from a direct (graphics) queue
        EXPECT_EQ(pd3d12CtxQueue->GetDesc().Type, D3D12_COMMAND_LIST_TYPE_DIRECT);
    }

    RunOpenXRFrames(Xr, pDevice, pGraphicsCtx, 30);
}

} // namespace
