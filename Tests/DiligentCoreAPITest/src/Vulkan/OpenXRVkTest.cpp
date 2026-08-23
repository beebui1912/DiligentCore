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

// Runs an OpenXR session on a Vulkan device created through the OpenXR runtime (XR_KHR_vulkan_enable2).
// See OpenXRTestHelpers.hpp for how the runtime is found.

// Brings in the Vulkan headers that RenderDeviceVk.h and openxr_platform.h need
#include "Vulkan/TestingEnvironmentVk.hpp"
#include "OpenXRTestHelpers.hpp"

#include "RenderDeviceVk.h"
#include "EngineFactoryVk.h"
#include "CommandQueueVk.h"
#include "OpenXRUtilities.h"

#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr_platform.h>

#include <iostream>
#include <vector>

#include "gtest/gtest.h"

using namespace Diligent;
using namespace Diligent::Testing;

namespace
{

TEST(OpenXRVk, SessionWithGraphicsContextNotFirst)
{
    GPUTestingEnvironment* pEnv = GPUTestingEnvironment::GetInstance();
    if (pEnv->GetDevice()->GetDeviceInfo().Type != RENDER_DEVICE_TYPE_VULKAN)
        GTEST_SKIP() << "Vulkan only";

    GPUTestingEnvironment::ScopedReset EnvironmentAutoReset;

    // The OpenXR instance has the extension that Diligent uses to create its Vulkan instance and device
    OpenXRTestInstance Xr;
    {
        const std::string SkipReason = Xr.Create(XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME);
        if (!SkipReason.empty())
            GTEST_SKIP() << SkipReason;
    }

    RefCntAutoPtr<IEngineFactoryVk> pFactory{pEnv->GetDevice()->GetEngineFactory(), IID_EngineFactoryVk};
    ASSERT_NE(pFactory, nullptr);

    Uint32 NumAdapters = 0;
    pFactory->EnumerateAdapters(Version{}, NumAdapters, nullptr);
    std::vector<GraphicsAdapterInfo> Adapters(NumAdapters);
    if (NumAdapters > 0)
        pFactory->EnumerateAdapters(Version{}, NumAdapters, Adapters.data());
    ASSERT_GT(NumAdapters, 0u);

    // Two immediate contexts: a compute context first, so that the graphics context has ContextId 1
    // while it uses queue 0 of its family. The graphics binding must name queue 0.
    const Uint8 ComputeFamily  = FindAdapterQueue(Adapters[0], COMMAND_QUEUE_TYPE_COMPUTE);
    const Uint8 GraphicsFamily = FindAdapterQueue(Adapters[0], COMMAND_QUEUE_TYPE_GRAPHICS);
    if (ComputeFamily == DEFAULT_QUEUE_ID || GraphicsFamily == DEFAULT_QUEUE_ID)
        GTEST_SKIP() << "The adapter has no separate compute queue family";

    ImmediateContextCreateInfo ContextCI[] = {
        {"XR compute", ComputeFamily},
        {"XR graphics", GraphicsFamily},
    };

    const OpenXRAttribs XRAttribs = Xr.GetAttribs();

    EngineVkCreateInfo EngineCI;
    EngineCI.pXRAttribs            = &XRAttribs;
    EngineCI.NumImmediateContexts  = _countof(ContextCI);
    EngineCI.pImmediateContextInfo = ContextCI;

    RefCntAutoPtr<IRenderDevice>  pDevice;
    RefCntAutoPtr<IDeviceContext> pContexts[2];
    {
        IDeviceContext* ppContexts[2] = {};
        pFactory->CreateDeviceAndContextsVk(EngineCI, &pDevice, ppContexts);
        pContexts[0].Attach(ppContexts[0]);
        pContexts[1].Attach(ppContexts[1]);
    }
    ASSERT_NE(pDevice, nullptr);
    ASSERT_NE(pContexts[0], nullptr);
    ASSERT_NE(pContexts[1], nullptr);
    // A new Vulkan immediate context starts with an open command buffer (it resets stale queries), so
    // submit it before the first FinishFrame(). There is no swapchain Present() that would flush it.
    for (IDeviceContext* pCtx : pContexts)
        pCtx->Flush();
    IDeviceContext* pGraphicsCtx = pContexts[1];

    std::cout << "    Device selected by the runtime: " << pDevice->GetAdapterInfo().Description << std::endl;

    // The graphics binding names the VkQueue that the context uses
    {
        RefCntAutoPtr<IDataBlob> pBinding;
        GetOpenXRGraphicsBinding(pDevice, pGraphicsCtx, &pBinding);
        ASSERT_NE(pBinding, nullptr);
        const XrGraphicsBindingVulkanKHR& Binding = *pBinding->GetConstDataPtr<XrGraphicsBindingVulkanKHR>();

        RefCntAutoPtr<ICommandQueueVk> pQueueVk{pGraphicsCtx->LockCommandQueue(), IID_CommandQueueVk};
        const uint32_t                 QueueIndex = pQueueVk->GetQueueIndex();
        const uint32_t                 Family     = pQueueVk->GetQueueFamilyIndex();
        pQueueVk.Release();
        pGraphicsCtx->UnlockCommandQueue();

        std::cout << "    Graphics context: ContextId " << Uint32{pGraphicsCtx->GetDesc().ContextId} << ", queue family " << Family
                  << ", queue index " << QueueIndex << "; binding queue family " << Binding.queueFamilyIndex
                  << ", queue index " << Binding.queueIndex << std::endl;
        EXPECT_EQ(Binding.queueFamilyIndex, Family);
        EXPECT_EQ(Binding.queueIndex, QueueIndex);
        EXPECT_EQ(Binding.queueIndex, 0u) << "The only queue of the graphics family is queue 0";
    }

    RunOpenXRFrames(Xr, pDevice, pGraphicsCtx, 30);
}

} // namespace
