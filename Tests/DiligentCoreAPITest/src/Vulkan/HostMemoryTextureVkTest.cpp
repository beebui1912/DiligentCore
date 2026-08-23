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

// Staging textures in an imported host allocation (VK_EXT_external_memory_host), which let two
// Vulkan devices that are not in one device group exchange data without a CPU copy.

#include "Vulkan/TestingEnvironmentVk.hpp"

#include "RenderDeviceVk.h"
#include "EngineFactoryVk.h"
#include "CommandQueueVk.h"
#include "AlignedMalloc.h"
#include "Align.hpp"

#include "volk.h"

#include <cstring>
#include <iostream>
#include <set>
#include <utility>
#include <vector>

#include "gtest/gtest.h"

using namespace Diligent;
using namespace Diligent::Testing;

namespace
{

constexpr Uint32 TexWidth  = 64;
constexpr Uint32 TexHeight = 32;

// Page-aligned host allocation that the test owns
class HostAllocation
{
public:
    HostAllocation(size_t Size, size_t Alignment) :
        m_Size{Size},
        m_pData{DILIGENT_ALIGNED_MALLOC(Size, Alignment, __FILE__, __LINE__)}
    {
        if (m_pData != nullptr)
            std::memset(m_pData, 0xCD, Size);
    }
    ~HostAllocation()
    {
        if (m_pData != nullptr)
            DILIGENT_ALIGNED_FREE(m_pData);
    }
    HostAllocation(const HostAllocation&) = delete;
    HostAllocation& operator=(const HostAllocation&) = delete;

    void*  GetData() const { return m_pData; }
    size_t GetSize() const { return m_Size; }

private:
    const size_t m_Size;
    void* const  m_pData;
};

std::vector<Uint8> MakeData(Uint8 Seed)
{
    std::vector<Uint8> Data(size_t{TexWidth} * TexHeight * 4);
    for (size_t i = 0; i < Data.size(); ++i)
        Data[i] = static_cast<Uint8>(i * 31 + Seed);
    return Data;
}

TextureDesc GetStagingDesc(CPU_ACCESS_FLAGS CPUAccess)
{
    TextureDesc Desc;
    Desc.Name           = "Host memory texture";
    Desc.Type           = RESOURCE_DIM_TEX_2D;
    Desc.Width          = TexWidth;
    Desc.Height         = TexHeight;
    Desc.Format         = TEX_FORMAT_RGBA8_UNORM;
    Desc.Usage          = USAGE_STAGING;
    Desc.CPUAccessFlags = CPUAccess;
    return Desc;
}

RefCntAutoPtr<ITexture> CreateFilledTexture(IRenderDevice* pDevice, const std::vector<Uint8>& Data)
{
    TextureDesc Desc = GetStagingDesc(CPU_ACCESS_NONE);
    Desc.Name        = "Source texture";
    Desc.Usage       = USAGE_IMMUTABLE;
    Desc.BindFlags   = BIND_SHADER_RESOURCE;

    TextureSubResData Sub{Data.data(), Uint64{TexWidth} * 4};
    TextureData       Init{&Sub, 1};

    RefCntAutoPtr<ITexture> pTex;
    pDevice->CreateTexture(Desc, &Init, &pTex);
    return pTex;
}

RefCntAutoPtr<ITexture> CreateDefaultTexture(IRenderDevice* pDevice)
{
    TextureDesc Desc = GetStagingDesc(CPU_ACCESS_NONE);
    Desc.Name        = "Destination texture";
    Desc.Usage       = USAGE_DEFAULT;
    Desc.BindFlags   = BIND_SHADER_RESOURCE;

    RefCntAutoPtr<ITexture> pTex;
    pDevice->CreateTexture(Desc, nullptr, &pTex);
    return pTex;
}

// Reads the texture back through an ordinary (engine-owned) staging texture
bool TextureContentsEqual(IRenderDevice* pDevice, IDeviceContext* pContext, ITexture* pTex, const std::vector<Uint8>& Expected)
{
    TextureDesc StagingDesc = GetStagingDesc(CPU_ACCESS_READ);
    StagingDesc.Name        = "Readback texture";

    RefCntAutoPtr<ITexture> pStaging;
    pDevice->CreateTexture(StagingDesc, nullptr, &pStaging);
    if (!pStaging)
        return false;

    CopyTextureAttribs Copy{pTex, RESOURCE_STATE_TRANSITION_MODE_TRANSITION, pStaging, RESOURCE_STATE_TRANSITION_MODE_TRANSITION};
    pContext->CopyTexture(Copy);
    pContext->WaitForIdle();

    MappedTextureSubresource Mapped;
    pContext->MapTextureSubresource(pStaging, 0, 0, MAP_READ, MAP_FLAG_DO_NOT_WAIT, nullptr, Mapped);
    bool         Equal    = Mapped.pData != nullptr;
    const size_t RowBytes = size_t{TexWidth} * 4;
    for (Uint32 Row = 0; Row < TexHeight && Equal; ++Row)
        Equal = std::memcmp(&Expected[Row * RowBytes], static_cast<const Uint8*>(Mapped.pData) + Row * Mapped.Stride, RowBytes) == 0;
    pContext->UnmapTextureSubresource(pStaging, 0, 0);
    return Equal;
}

// The data of a staging texture is tightly packed: row after row, starting at offset 0
bool HostMemoryEqual(const HostAllocation& Memory, const std::vector<Uint8>& Expected)
{
    return Memory.GetSize() >= Expected.size() && std::memcmp(Memory.GetData(), Expected.data(), Expected.size()) == 0;
}

// A Vulkan device on a hardware adapter other than the one of the testing environment
bool CreateDeviceOnOtherAdapter(IRenderDevice* pEnvDevice, RefCntAutoPtr<IRenderDevice>& pDevice, RefCntAutoPtr<IDeviceContext>& pContext)
{
    RefCntAutoPtr<IEngineFactoryVk> pFactory{pEnvDevice->GetEngineFactory(), IID_EngineFactoryVk};
    if (!pFactory)
        return false;

    const GraphicsAdapterInfo& EnvAdapter = pEnvDevice->GetAdapterInfo();

    Uint32 NumAdapters = 0;
    pFactory->EnumerateAdapters(Version{}, NumAdapters, nullptr);
    std::vector<GraphicsAdapterInfo> Adapters(NumAdapters);
    if (NumAdapters > 0)
        pFactory->EnumerateAdapters(Version{}, NumAdapters, Adapters.data());

    for (Uint32 i = 0; i < NumAdapters; ++i)
    {
        const GraphicsAdapterInfo& Adapter = Adapters[i];
        if (Adapter.Type == ADAPTER_TYPE_SOFTWARE ||
            (Adapter.VendorId == EnvAdapter.VendorId && Adapter.DeviceId == EnvAdapter.DeviceId))
            continue;

        // No immediate contexts: the device creates the default graphics context
        EngineVkCreateInfo EngineCI;
        EngineCI.AdapterId = i;
        pFactory->CreateDeviceAndContextsVk(EngineCI, &pDevice, &pContext);
        if (pDevice && pContext)
            return true;
        pDevice.Release();
        pContext.Release();
    }
    return false;
}

// The GPU writes the application's allocation directly: the engine never maps or copies it
TEST(HostMemoryTextureVk, GpuWritesHostMemory)
{
    GPUTestingEnvironment* pEnv     = GPUTestingEnvironment::GetInstance();
    IRenderDevice*         pDevice  = pEnv->GetDevice();
    IDeviceContext*        pContext = pEnv->GetDeviceContext();
    if (pDevice->GetDeviceInfo().Type != RENDER_DEVICE_TYPE_VULKAN)
        GTEST_SKIP() << "Vulkan only";

    GPUTestingEnvironment::ScopedReset EnvironmentAutoReset;

    RefCntAutoPtr<IRenderDeviceVk> pDeviceVk{pDevice, IID_RenderDeviceVk};

    const TextureDesc ReadbackDesc = GetStagingDesc(CPU_ACCESS_READ);
    Uint64            Size = 0, Alignment = 0;
    if (!pDeviceVk->GetHostMemoryTextureRequirements(ReadbackDesc, Size, Alignment))
        GTEST_SKIP() << "The device can not import host memory (VK_EXT_external_memory_host)";

    EXPECT_GE(Size, Uint64{TexWidth} * TexHeight * 4);
    EXPECT_EQ(Size % Alignment, 0u);
    EXPECT_TRUE(IsPowerOfTwo(Alignment));
    std::cout << "    Host memory for a " << TexWidth << "x" << TexHeight << " RGBA8 texture: " << Size << " bytes, alignment " << Alignment << std::endl;

    // Non-staging descriptions are not supported
    {
        TextureDesc DefaultDesc = ReadbackDesc;
        DefaultDesc.Usage       = USAGE_DEFAULT;
        Uint64 S = 0, A = 0;
        EXPECT_FALSE(pDeviceVk->GetHostMemoryTextureRequirements(DefaultDesc, S, A));
    }

    const std::vector<Uint8> Data = MakeData(7);
    {
        HostAllocation Memory{static_cast<size_t>(Size), static_cast<size_t>(Alignment)};
        ASSERT_NE(Memory.GetData(), nullptr);

        RefCntAutoPtr<ITexture> pHostTex;
        pDeviceVk->CreateTextureFromHostMemory(ReadbackDesc, Memory.GetData(), nullptr, &pHostTex);
        ASSERT_NE(pHostTex, nullptr);

        RefCntAutoPtr<ITexture> pSrc = CreateFilledTexture(pDevice, Data);
        ASSERT_NE(pSrc, nullptr);

        EXPECT_FALSE(HostMemoryEqual(Memory, Data));

        CopyTextureAttribs Copy{pSrc, RESOURCE_STATE_TRANSITION_MODE_TRANSITION, pHostTex, RESOURCE_STATE_TRANSITION_MODE_TRANSITION};
        pContext->CopyTexture(Copy);
        pContext->WaitForIdle();

        EXPECT_TRUE(HostMemoryEqual(Memory, Data)) << "The GPU copy did not write the application's allocation";

        // Mapping returns the application's allocation itself
        MappedTextureSubresource Mapped;
        pContext->MapTextureSubresource(pHostTex, 0, 0, MAP_READ, MAP_FLAG_DO_NOT_WAIT, nullptr, Mapped);
        EXPECT_EQ(Mapped.pData, Memory.GetData());
        pContext->UnmapTextureSubresource(pHostTex, 0, 0);

        // Without an owner, the allocation must outlive the imported memory: release the texture and idle the GPU
        pHostTex.Release();
        pSrc.Release();
        pDevice->IdleGPU();
    }
}

// The application writes the allocation and the GPU reads it: an upload texture in host memory
TEST(HostMemoryTextureVk, GpuReadsHostMemory)
{
    GPUTestingEnvironment* pEnv     = GPUTestingEnvironment::GetInstance();
    IRenderDevice*         pDevice  = pEnv->GetDevice();
    IDeviceContext*        pContext = pEnv->GetDeviceContext();
    if (pDevice->GetDeviceInfo().Type != RENDER_DEVICE_TYPE_VULKAN)
        GTEST_SKIP() << "Vulkan only";

    GPUTestingEnvironment::ScopedReset EnvironmentAutoReset;

    RefCntAutoPtr<IRenderDeviceVk> pDeviceVk{pDevice, IID_RenderDeviceVk};

    const TextureDesc UploadDesc = GetStagingDesc(CPU_ACCESS_WRITE);
    Uint64            Size = 0, Alignment = 0;
    if (!pDeviceVk->GetHostMemoryTextureRequirements(UploadDesc, Size, Alignment))
        GTEST_SKIP() << "The device can not import host memory (VK_EXT_external_memory_host)";

    const std::vector<Uint8> Data = MakeData(101);

    HostAllocation Memory{static_cast<size_t>(Size), static_cast<size_t>(Alignment)};
    ASSERT_NE(Memory.GetData(), nullptr);
    std::memcpy(Memory.GetData(), Data.data(), Data.size());

    RefCntAutoPtr<ITexture> pHostTex;
    pDeviceVk->CreateTextureFromHostMemory(UploadDesc, Memory.GetData(), nullptr, &pHostTex);
    ASSERT_NE(pHostTex, nullptr);

    RefCntAutoPtr<ITexture> pDst = CreateDefaultTexture(pDevice);
    ASSERT_NE(pDst, nullptr);

    CopyTextureAttribs Copy{pHostTex, RESOURCE_STATE_TRANSITION_MODE_TRANSITION, pDst, RESOURCE_STATE_TRANSITION_MODE_TRANSITION};
    pContext->CopyTexture(Copy);

    EXPECT_TRUE(TextureContentsEqual(pDevice, pContext, pDst, Data));

    pHostTex.Release();
    pDevice->IdleGPU();
}

// Two devices on different adapters import one allocation. One GPU writes it, the other reads it.
// The CPU only waits for the first device; it never reads or writes the data.
TEST(HostMemoryTextureVk, TwoDevices)
{
    GPUTestingEnvironment* pEnv     = GPUTestingEnvironment::GetInstance();
    IRenderDevice*         pDeviceA = pEnv->GetDevice();
    IDeviceContext*        pCtxA    = pEnv->GetDeviceContext();
    if (pDeviceA->GetDeviceInfo().Type != RENDER_DEVICE_TYPE_VULKAN)
        GTEST_SKIP() << "Vulkan only";

    GPUTestingEnvironment::ScopedReset EnvironmentAutoReset;

    RefCntAutoPtr<IRenderDevice>  pDeviceB;
    RefCntAutoPtr<IDeviceContext> pCtxB;
    if (!CreateDeviceOnOtherAdapter(pDeviceA, pDeviceB, pCtxB))
        GTEST_SKIP() << "No second hardware adapter";
    std::cout << "    Device A: " << pDeviceA->GetAdapterInfo().Description << "\n    Device B: " << pDeviceB->GetAdapterInfo().Description << std::endl;

    RefCntAutoPtr<IRenderDeviceVk> pDeviceAVk{pDeviceA, IID_RenderDeviceVk};
    RefCntAutoPtr<IRenderDeviceVk> pDeviceBVk{pDeviceB, IID_RenderDeviceVk};

    const TextureDesc ReadbackDesc = GetStagingDesc(CPU_ACCESS_READ);
    const TextureDesc UploadDesc   = GetStagingDesc(CPU_ACCESS_WRITE);

    Uint64 SizeA = 0, AlignA = 0, SizeB = 0, AlignB = 0;
    if (!pDeviceAVk->GetHostMemoryTextureRequirements(ReadbackDesc, SizeA, AlignA) ||
        !pDeviceBVk->GetHostMemoryTextureRequirements(UploadDesc, SizeB, AlignB))
        GTEST_SKIP() << "A device can not import host memory (VK_EXT_external_memory_host)";

    // Both directions: A -> B, then B -> A
    const std::pair<int, int> Directions[] = {{0, 1}, {1, 0}};
    for (const std::pair<int, int>& Dir : Directions)
    {
        IRenderDevice*   pDevices[]   = {pDeviceA, pDeviceB};
        IRenderDeviceVk* pDevicesVk[] = {pDeviceAVk, pDeviceBVk};
        IDeviceContext*  pContexts[]  = {pCtxA, pCtxB};

        IRenderDevice*   pSrcDevice   = pDevices[Dir.first];
        IRenderDeviceVk* pSrcDeviceVk = pDevicesVk[Dir.first];
        IDeviceContext*  pSrcCtx      = pContexts[Dir.first];
        IRenderDevice*   pDstDevice   = pDevices[Dir.second];
        IRenderDeviceVk* pDstDeviceVk = pDevicesVk[Dir.second];
        IDeviceContext*  pDstCtx      = pContexts[Dir.second];

        const size_t Size      = static_cast<size_t>((std::max)(SizeA, SizeB));
        const size_t Alignment = static_cast<size_t>((std::max)(AlignA, AlignB));

        const std::vector<Uint8> Data = MakeData(static_cast<Uint8>(17 + Dir.first));
        {
            HostAllocation Memory{Size, Alignment};
            ASSERT_NE(Memory.GetData(), nullptr);

            RefCntAutoPtr<ITexture> pSrcView;
            RefCntAutoPtr<ITexture> pDstView;
            pSrcDeviceVk->CreateTextureFromHostMemory(ReadbackDesc, Memory.GetData(), nullptr, &pSrcView);
            pDstDeviceVk->CreateTextureFromHostMemory(UploadDesc, Memory.GetData(), nullptr, &pDstView);
            ASSERT_NE(pSrcView, nullptr);
            ASSERT_NE(pDstView, nullptr);

            RefCntAutoPtr<ITexture> pSrc = CreateFilledTexture(pSrcDevice, Data);
            RefCntAutoPtr<ITexture> pDst = CreateDefaultTexture(pDstDevice);
            ASSERT_NE(pSrc, nullptr);
            ASSERT_NE(pDst, nullptr);

            // Source GPU: frame -> shared host memory
            {
                CopyTextureAttribs Copy{pSrc, RESOURCE_STATE_TRANSITION_MODE_TRANSITION, pSrcView, RESOURCE_STATE_TRANSITION_MODE_TRANSITION};
                pSrcCtx->CopyTexture(Copy);
            }
            // The CPU waits for the source GPU (no data access)
            pSrcCtx->WaitForIdle();

            // Destination GPU: shared host memory -> texture
            {
                CopyTextureAttribs Copy{pDstView, RESOURCE_STATE_TRANSITION_MODE_TRANSITION, pDst, RESOURCE_STATE_TRANSITION_MODE_TRANSITION};
                pDstCtx->CopyTexture(Copy);
            }

            EXPECT_TRUE(TextureContentsEqual(pDstDevice, pDstCtx, pDst, Data))
                << "Device " << Dir.second << " does not see the data that device " << Dir.first << " wrote";
            EXPECT_TRUE(HostMemoryEqual(Memory, Data));

            pSrcView.Release();
            pDstView.Release();
            pSrcDevice->IdleGPU();
            pDstDevice->IdleGPU();
        }
    }

    pCtxB->Flush();
    pCtxB->FinishFrame();
}

// The queue index that ICommandQueueVk reports (and OpenXR receives) is the one of the VkQueue in use,
// and contexts that use different queue indices have different VkQueues
TEST(HostMemoryTextureVk, QueueIndexMatchesVkQueue)
{
    GPUTestingEnvironment* pEnv    = GPUTestingEnvironment::GetInstance();
    IRenderDevice*         pDevice = pEnv->GetDevice();
    if (pDevice->GetDeviceInfo().Type != RENDER_DEVICE_TYPE_VULKAN)
        GTEST_SKIP() << "Vulkan only";

    RefCntAutoPtr<IRenderDeviceVk> pDeviceVk{pDevice, IID_RenderDeviceVk};
    const VkDevice                 vkDevice = pDeviceVk->GetVkDevice();

    std::set<std::pair<uint32_t, uint32_t>> FamilyAndIndex;
    std::set<VkQueue>                       Queues;
    for (Uint32 ctx = 0; ctx < pEnv->GetNumImmediateContexts(); ++ctx)
    {
        IDeviceContext* pContext = pEnv->GetDeviceContext(ctx);

        RefCntAutoPtr<ICommandQueueVk> pQueueVk{pContext->LockCommandQueue(), IID_CommandQueueVk};
        ASSERT_NE(pQueueVk, nullptr);
        const uint32_t Family  = pQueueVk->GetQueueFamilyIndex();
        const uint32_t Index   = pQueueVk->GetQueueIndex();
        const VkQueue  vkQueue = pQueueVk->GetVkQueue();
        pQueueVk.Release();
        pContext->UnlockCommandQueue();

        VkQueue vkExpected = VK_NULL_HANDLE;
        vkGetDeviceQueue(vkDevice, Family, Index, &vkExpected);
        EXPECT_EQ(vkQueue, vkExpected) << "Context " << ctx << ": family " << Family << ", index " << Index;
        std::cout << "    Context " << ctx << " '" << pContext->GetDesc().Name << "': queue family " << Family << ", queue index " << Index << std::endl;

        FamilyAndIndex.emplace(Family, Index);
        Queues.insert(vkQueue);
    }
    EXPECT_EQ(Queues.size(), FamilyAndIndex.size()) << "Contexts with different queue indices must not share a VkQueue";
}

} // namespace
