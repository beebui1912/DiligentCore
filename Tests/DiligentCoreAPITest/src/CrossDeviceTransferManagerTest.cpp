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

// CrossDeviceTransferManager between two devices on different adapters (unlinked multi-GPU):
// frames go through shared memory with GPU copies, or through the CPU.

#include "GPUTestingEnvironment.hpp"
#include "CrossDeviceTransferManager.hpp"

#if VULKAN_SUPPORTED
#    include "Vulkan/TestingEnvironmentVk.hpp"
#    include "EngineFactoryVk.h"
#endif

#if D3D12_SUPPORTED
#    include <d3d12.h>
#    include "EngineFactoryD3D12.h"
#    include "RenderDeviceD3D12.h"
#endif

#include <chrono>
#include <cstring>
#include <iostream>
#include <vector>

#include "gtest/gtest.h"

using namespace Diligent;
using namespace Diligent::Testing;

namespace
{

using TransferMode = CrossDeviceTransferManager::TransferMode;

// A device of the same backend on a hardware adapter other than the one of the testing environment
bool CreateDeviceOnOtherAdapter(IRenderDevice* pEnvDevice, RefCntAutoPtr<IRenderDevice>& pDevice, RefCntAutoPtr<IDeviceContext>& pContext)
{
    const GraphicsAdapterInfo& EnvAdapter = pEnvDevice->GetAdapterInfo();

    auto IsOtherHardwareAdapter = [&EnvAdapter](const GraphicsAdapterInfo& Adapter) {
        return Adapter.Type != ADAPTER_TYPE_SOFTWARE &&
            (Adapter.VendorId != EnvAdapter.VendorId || Adapter.DeviceId != EnvAdapter.DeviceId);
    };

    switch (pEnvDevice->GetDeviceInfo().Type)
    {
#if VULKAN_SUPPORTED
        case RENDER_DEVICE_TYPE_VULKAN:
        {
            RefCntAutoPtr<IEngineFactoryVk> pFactory{pEnvDevice->GetEngineFactory(), IID_EngineFactoryVk};
            Uint32                          NumAdapters = 0;
            pFactory->EnumerateAdapters(Version{}, NumAdapters, nullptr);
            std::vector<GraphicsAdapterInfo> Adapters(NumAdapters);
            if (NumAdapters > 0)
                pFactory->EnumerateAdapters(Version{}, NumAdapters, Adapters.data());
            for (Uint32 i = 0; i < NumAdapters; ++i)
            {
                if (!IsOtherHardwareAdapter(Adapters[i]))
                    continue;
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
#endif

#if D3D12_SUPPORTED
        case RENDER_DEVICE_TYPE_D3D12:
        {
            RefCntAutoPtr<IEngineFactoryD3D12> pFactory{pEnvDevice->GetEngineFactory(), IID_EngineFactoryD3D12};
            const Version                      MinVersion{11, 0};
            Uint32                             NumAdapters = 0;
            pFactory->EnumerateAdapters(MinVersion, NumAdapters, nullptr);
            std::vector<GraphicsAdapterInfo> Adapters(NumAdapters);
            if (NumAdapters > 0)
                pFactory->EnumerateAdapters(MinVersion, NumAdapters, Adapters.data());
            for (Uint32 i = 0; i < NumAdapters; ++i)
            {
                if (!IsOtherHardwareAdapter(Adapters[i]))
                    continue;
                EngineD3D12CreateInfo EngineCI;
                EngineCI.AdapterId          = i;
                EngineCI.GraphicsAPIVersion = MinVersion;
                pFactory->CreateDeviceAndContextsD3D12(EngineCI, &pDevice, &pContext);
                if (pDevice && pContext)
                    return true;
                pDevice.Release();
                pContext.Release();
            }
            return false;
        }
#endif

        default:
            return false;
    }
}

TextureDesc GetFrameDesc(Uint32 Width, Uint32 Height)
{
    TextureDesc Desc;
    Desc.Name      = "Frame";
    Desc.Type      = RESOURCE_DIM_TEX_2D;
    Desc.Width     = Width;
    Desc.Height    = Height;
    Desc.Format    = TEX_FORMAT_RGBA8_UNORM;
    Desc.Usage     = USAGE_DEFAULT;
    Desc.BindFlags = BIND_SHADER_RESOURCE;
    return Desc;
}

std::vector<Uint8> MakeFrameData(const TextureDesc& Desc, Uint32 Frame)
{
    std::vector<Uint8> Data(size_t{Desc.Width} * Desc.Height * 4);
    for (size_t i = 0; i < Data.size(); ++i)
        Data[i] = static_cast<Uint8>(i * 7 + Frame * 53 + 1);
    return Data;
}

bool TextureContentsEqual(IRenderDevice* pDevice, IDeviceContext* pContext, ITexture* pTex, const std::vector<Uint8>& Expected)
{
    TextureDesc StagingDesc    = pTex->GetDesc();
    StagingDesc.Name           = "Readback texture";
    StagingDesc.BindFlags      = BIND_NONE;
    StagingDesc.Usage          = USAGE_STAGING;
    StagingDesc.CPUAccessFlags = CPU_ACCESS_READ;

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
    const size_t RowBytes = size_t{StagingDesc.Width} * 4;
    for (Uint32 Row = 0; Row < StagingDesc.Height && Equal; ++Row)
        Equal = std::memcmp(&Expected[Row * RowBytes], static_cast<const Uint8*>(Mapped.pData) + Row * Mapped.Stride, RowBytes) == 0;
    pContext->UnmapTextureSubresource(pStaging, 0, 0);
    return Equal;
}

struct TransferResult
{
    TransferMode Mode               = TransferMode::None;
    Uint32       NumTransfers       = 0;
    Uint32       NumCorrectFrames   = 0;
    double       MillisecondsPerFrm = 0;
};

// Sends NumFrames frames with different contents from the source device to the destination device.
// If VerifyFrames is true, every transferred frame is read back and compared on the destination device.
TransferResult RunTransfer(IRenderDevice*     pSrcDevice,
                           IDeviceContext*    pSrcCtx,
                           IRenderDevice*     pDstDevice,
                           IDeviceContext*    pDstCtx,
                           const TextureDesc& FrameDesc,
                           Uint32             NumFrames,
                           bool               AllowSharedMemory,
                           bool               VerifyFrames)
{
    TransferResult Result;

    RefCntAutoPtr<ITexture> pSrc;
    RefCntAutoPtr<ITexture> pDst;
    pSrcDevice->CreateTexture(FrameDesc, nullptr, &pSrc);
    pDstDevice->CreateTexture(FrameDesc, nullptr, &pDst);
    if (!pSrc || !pDst)
        return Result;

    std::vector<std::vector<Uint8>> Frames(NumFrames);
    for (Uint32 f = 0; f < NumFrames; ++f)
        Frames[f] = MakeFrameData(FrameDesc, f);

    constexpr Uint32           Latency = 2;
    CrossDeviceTransferManager TransferMgr;
    TransferMgr.Init(pSrcDevice, pDstDevice, FrameDesc, Latency, AllowSharedMemory);
    Result.Mode = TransferMgr.GetTransferMode();

    const auto StartTime = std::chrono::high_resolution_clock::now();
    for (Uint32 f = 0; f < NumFrames; ++f)
    {
        // "Render" the frame on the source device
        const Box         Region{0, FrameDesc.Width, 0, FrameDesc.Height};
        TextureSubResData SubRes{Frames[f].data(), Uint64{FrameDesc.Width} * 4};
        pSrcCtx->UpdateTexture(pSrc, 0, 0, Region, SubRes, RESOURCE_STATE_TRANSITION_MODE_TRANSITION, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

        TransferMgr.CopySourceFrame(pSrcCtx, pSrc, f);
        if (TransferMgr.TransferReadyFrame(pSrcCtx, pDstCtx, pDst, f))
        {
            ++Result.NumTransfers;
            if (VerifyFrames && TextureContentsEqual(pDstDevice, pDstCtx, pDst, Frames[f - (Latency - 1)]))
                ++Result.NumCorrectFrames;
        }
    }
    pDstCtx->WaitForIdle();
    const auto EndTime = std::chrono::high_resolution_clock::now();

    Result.MillisecondsPerFrm = std::chrono::duration<double, std::milli>(EndTime - StartTime).count() / NumFrames;

    TransferMgr.Reset();
    pSrcCtx->WaitForIdle();
    pSrcCtx->FinishFrame();
    pDstCtx->FinishFrame();
    return Result;
}

TransferMode GetExpectedSharedMode(RENDER_DEVICE_TYPE Type)
{
    return Type == RENDER_DEVICE_TYPE_VULKAN ? TransferMode::SharedHostMemory :
                                               TransferMode::SharedCrossAdapterTexture;
}

class CrossDeviceTransferManagerTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        GPUTestingEnvironment* pEnv = GPUTestingEnvironment::GetInstance();
        m_pDeviceA                  = pEnv->GetDevice();
        m_pCtxA                     = pEnv->GetDeviceContext();

        const RENDER_DEVICE_TYPE Type = m_pDeviceA->GetDeviceInfo().Type;
        if (Type != RENDER_DEVICE_TYPE_VULKAN && Type != RENDER_DEVICE_TYPE_D3D12)
            GTEST_SKIP() << "Vulkan and Direct3D12 only";

        if (!CreateDeviceOnOtherAdapter(m_pDeviceA, m_pDeviceB, m_pCtxB))
            GTEST_SKIP() << "No second hardware adapter";

        std::cout << "    Device A: " << m_pDeviceA->GetAdapterInfo().Description
                  << "\n    Device B: " << m_pDeviceB->GetAdapterInfo().Description << std::endl;
    }

    void TearDown() override
    {
        m_pCtxB.Release();
        m_pDeviceB.Release();
        // Resets the environment's device when it goes out of scope
        GPUTestingEnvironment::ScopedReset EnvironmentAutoReset;
    }

    IRenderDevice*                m_pDeviceA = nullptr;
    IDeviceContext*               m_pCtxA    = nullptr;
    RefCntAutoPtr<IRenderDevice>  m_pDeviceB;
    RefCntAutoPtr<IDeviceContext> m_pCtxB;
};

// Every frame arrives intact through shared memory, in both directions
TEST_F(CrossDeviceTransferManagerTest, SharedMemory)
{
    const TextureDesc  FrameDesc    = GetFrameDesc(256, 128);
    const TransferMode ExpectedMode = GetExpectedSharedMode(m_pDeviceA->GetDeviceInfo().Type);
    constexpr Uint32   NumFrames    = 8;

    IRenderDevice*  pDevices[]  = {m_pDeviceA, m_pDeviceB};
    IDeviceContext* pContexts[] = {m_pCtxA, m_pCtxB};
    for (int Src = 0; Src < 2; ++Src)
    {
        const int Dst = 1 - Src;

        const TransferResult Result = RunTransfer(pDevices[Src], pContexts[Src], pDevices[Dst], pContexts[Dst], FrameDesc, NumFrames,
                                                  /*AllowSharedMemory = */ true, /*VerifyFrames = */ true);
        std::cout << "    " << (Src == 0 ? "A -> B" : "B -> A") << ": " << CrossDeviceTransferManager::GetTransferModeString(Result.Mode)
                  << ", " << Result.NumCorrectFrames << "/" << Result.NumTransfers << " frames correct" << std::endl;

        EXPECT_EQ(Result.Mode, ExpectedMode) << "The devices did not share memory";
        EXPECT_EQ(Result.NumTransfers, NumFrames - 1);
        EXPECT_EQ(Result.NumCorrectFrames, Result.NumTransfers);
    }
}

// The CPU copy fallback still works when sharing is not allowed
TEST_F(CrossDeviceTransferManagerTest, CpuCopy)
{
    const TextureDesc FrameDesc = GetFrameDesc(256, 128);
    constexpr Uint32  NumFrames = 8;

    const TransferResult Result = RunTransfer(m_pDeviceA, m_pCtxA, m_pDeviceB, m_pCtxB, FrameDesc, NumFrames,
                                              /*AllowSharedMemory = */ false, /*VerifyFrames = */ true);
    EXPECT_EQ(Result.Mode, TransferMode::CpuCopy);
    EXPECT_EQ(Result.NumTransfers, NumFrames - 1);
    EXPECT_EQ(Result.NumCorrectFrames, Result.NumTransfers);
}

// Informational: time per 1920x1080 RGBA8 frame through shared memory and through the CPU
TEST_F(CrossDeviceTransferManagerTest, Timing)
{
    const TextureDesc FrameDesc = GetFrameDesc(1920, 1080);
    constexpr Uint32  NumFrames = 60;

    for (const bool AllowShared : {true, false})
    {
        const TransferResult Result = RunTransfer(m_pDeviceA, m_pCtxA, m_pDeviceB, m_pCtxB, FrameDesc, NumFrames, AllowShared, /*VerifyFrames = */ false);
        std::cout << "    A -> B, 1920x1080 RGBA8, " << NumFrames << " frames, " << CrossDeviceTransferManager::GetTransferModeString(Result.Mode)
                  << ": " << Result.MillisecondsPerFrm << " ms per frame" << std::endl;
        EXPECT_EQ(Result.NumTransfers, NumFrames - 1);
    }
}

} // namespace
