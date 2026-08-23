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

// Unlinked multi-GPU: two devices of the same backend.
// * CreateCrossDeviceFence(): fences that two devices share, so that one device's queue waits on the GPU
//   for a value that the other device signals.
// * CrossDeviceTransferManager: frames go through shared memory with GPU copies, or through the CPU, and the
//   devices synchronize on the GPU (shared fences) or through the CPU.
// The second device is on another adapter, or (Vulkan only) on the adapter of the testing environment.

#include "GPUTestingEnvironment.hpp"
#include "CrossDeviceTransferManager.hpp"
#include "GraphicsUtilities.h"

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
#include <thread>
#include <vector>

#include "gtest/gtest.h"

using namespace Diligent;
using namespace Diligent::Testing;

namespace
{

using TransferMode = CrossDeviceTransferManager::TransferMode;

enum class SecondDeviceAdapter
{
    // A hardware adapter other than the one of the testing environment
    Other,

    // The adapter of the testing environment. Not used with Direct3D12, where a second device on the
    // same adapter is the same ID3D12Device.
    Same
};

// A device of the same backend as the testing environment, on the requested adapter
bool CreateSecondDevice(IRenderDevice* pEnvDevice, SecondDeviceAdapter Adapter, RefCntAutoPtr<IRenderDevice>& pDevice, RefCntAutoPtr<IDeviceContext>& pContext)
{
    const GraphicsAdapterInfo& EnvAdapter = pEnvDevice->GetAdapterInfo();

    auto IsRequestedAdapter = [&](const GraphicsAdapterInfo& Info) {
        if (Info.Type == ADAPTER_TYPE_SOFTWARE)
            return false;
        const bool IsEnvAdapter = Info.VendorId == EnvAdapter.VendorId && Info.DeviceId == EnvAdapter.DeviceId;
        return Adapter == SecondDeviceAdapter::Same ? IsEnvAdapter : !IsEnvAdapter;
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
                if (!IsRequestedAdapter(Adapters[i]))
                    continue;
                EngineVkCreateInfo EngineCI;
                EngineCI.AdapterId = i;
                // Shared fences are timeline semaphores
                EngineCI.Features.NativeFence = DEVICE_FEATURE_STATE_OPTIONAL;
                pFactory->CreateDeviceAndContextsVk(EngineCI, &pDevice, &pContext);
                if (pDevice && pContext)
                {
                    // A new Vulkan immediate context has an open command buffer (stale query resets): submit it,
                    // so that a test that does not use the context releases it cleanly
                    pContext->Flush();
                    return true;
                }
                pDevice.Release();
                pContext.Release();
            }
            return false;
        }
#endif

#if D3D12_SUPPORTED
        case RENDER_DEVICE_TYPE_D3D12:
        {
            if (Adapter == SecondDeviceAdapter::Same)
                return false;

            RefCntAutoPtr<IEngineFactoryD3D12> pFactory{pEnvDevice->GetEngineFactory(), IID_EngineFactoryD3D12};
            const Version                      MinVersion{11, 0};
            Uint32                             NumAdapters = 0;
            pFactory->EnumerateAdapters(MinVersion, NumAdapters, nullptr);
            std::vector<GraphicsAdapterInfo> Adapters(NumAdapters);
            if (NumAdapters > 0)
                pFactory->EnumerateAdapters(MinVersion, NumAdapters, Adapters.data());
            for (Uint32 i = 0; i < NumAdapters; ++i)
            {
                if (!IsRequestedAdapter(Adapters[i]))
                    continue;
                EngineD3D12CreateInfo EngineCI;
                EngineCI.AdapterId            = i;
                EngineCI.GraphicsAPIVersion   = MinVersion;
                EngineCI.Features.NativeFence = DEVICE_FEATURE_STATE_OPTIONAL;
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

// Polls the fence, so that a test fails instead of hanging if the value is never reached
bool WaitForFenceValue(IFence* pFence, Uint64 Value, std::chrono::milliseconds Timeout)
{
    const auto StartTime = std::chrono::steady_clock::now();
    while (pFence->GetCompletedValue() < Value)
    {
        if (std::chrono::steady_clock::now() - StartTime > Timeout)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    return true;
}

// The devices share a fence and take turns: the waiting device's queue waits on the GPU for a value of the
// shared fence, then signals a fence of its own. While the other device has not signaled the value, the
// waiting device's fence must not complete; once it has, it must complete without any CPU wait in between.
void TestGpuWaitsForOtherDevice(IRenderDevice* pDeviceA, IDeviceContext* pCtxA, IRenderDevice* pDeviceB, IDeviceContext* pCtxB)
{
    FenceDesc SharedDesc;
    SharedDesc.Name = "Cross-device fence";
    SharedDesc.Type = FENCE_TYPE_GENERAL;

    RefCntAutoPtr<IFence> pSharedA; // Fence of device A
    RefCntAutoPtr<IFence> pSharedB; // Fence of device B, with the same value
    ASSERT_TRUE(CreateCrossDeviceFence(pDeviceA, pDeviceB, SharedDesc, &pSharedA, &pSharedB));
    ASSERT_NE(pSharedA, nullptr);
    ASSERT_NE(pSharedB, nullptr);

    FenceDesc DoneDesc;
    DoneDesc.Type = FENCE_TYPE_GENERAL;
    RefCntAutoPtr<IFence> pDone[2];
    DoneDesc.Name = "Device A done";
    pDeviceA->CreateFence(DoneDesc, &pDone[0]);
    DoneDesc.Name = "Device B done";
    pDeviceB->CreateFence(DoneDesc, &pDone[1]);
    ASSERT_TRUE(pDone[0] && pDone[1]);

    IDeviceContext* const pContexts[] = {pCtxA, pCtxB};
    IFence* const         pShared[]   = {pSharedA, pSharedB};

    // Odd values: B waits for A. Even values: A waits for B.
    constexpr Uint64 NumRounds = 4;
    for (Uint64 Value = 1; Value <= NumRounds; ++Value)
    {
        const int Waiter   = Value % 2 == 1 ? 1 : 0;
        const int Signaler = 1 - Waiter;
        const char* Names[] = {"A", "B"};

        pContexts[Waiter]->DeviceWaitForFence(pShared[Waiter], Value);
        pContexts[Waiter]->EnqueueSignal(pDone[Waiter], Value);
        pContexts[Waiter]->Flush();

        // The waiting queue is blocked by the GPU wait
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
        EXPECT_LT(pDone[Waiter]->GetCompletedValue(), Value) << "Device " << Names[Waiter] << " did not wait for device " << Names[Signaler];

        pContexts[Signaler]->EnqueueSignal(pShared[Signaler], Value);
        pContexts[Signaler]->Flush();

        // The signal of the other device releases the wait
        const bool Released = WaitForFenceValue(pDone[Waiter], Value, std::chrono::seconds{5});
        EXPECT_TRUE(Released) << "The signal of device " << Names[Signaler] << " did not reach device " << Names[Waiter];
        if (!Released)
        {
            // Unblock the waiting queue, so that the devices can be released
            pShared[Waiter]->Signal(Value);
            pDone[Waiter]->Wait(Value);
        }
        EXPECT_GE(pShared[Waiter]->GetCompletedValue(), Value);
        EXPECT_GE(pShared[Signaler]->GetCompletedValue(), Value);
    }

    std::cout << "    " << NumRounds << " rounds: each device's queue waited on the GPU for the other device's signal" << std::endl;

    pCtxA->WaitForIdle();
    pCtxB->WaitForIdle();
    pCtxA->FinishFrame();
    pCtxB->FinishFrame();
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
    bool         GpuSync            = false;
    Uint32       NumTransfers       = 0;
    Uint32       NumCorrectFrames   = 0;
    double       MillisecondsPerFrm = 0;
};

const char* GetSyncString(bool GpuSync)
{
    return GpuSync ? "GPU waits (shared fences)" : "CPU waits";
}

// Sends NumFrames frames with different contents from the source device to the destination device.
// If VerifyFrames is true, every transferred frame is read back and compared on the destination device.
TransferResult RunTransfer(IRenderDevice*     pSrcDevice,
                           IDeviceContext*    pSrcCtx,
                           IRenderDevice*     pDstDevice,
                           IDeviceContext*    pDstCtx,
                           const TextureDesc& FrameDesc,
                           Uint32             NumFrames,
                           bool               AllowSharedMemory,
                           bool               AllowGpuSync,
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
    TransferMgr.Init(pSrcDevice, pDstDevice, FrameDesc, Latency, AllowSharedMemory, AllowGpuSync);
    Result.Mode    = TransferMgr.GetTransferMode();
    Result.GpuSync = TransferMgr.UsesGpuSynchronization();

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

    pSrcCtx->WaitForIdle();
    TransferMgr.Reset();
    pSrcCtx->FinishFrame();
    pDstCtx->FinishFrame();
    return Result;
}

TransferMode GetExpectedSharedMode(RENDER_DEVICE_TYPE Type)
{
    return Type == RENDER_DEVICE_TYPE_VULKAN ? TransferMode::SharedHostMemory :
                                               TransferMode::SharedCrossAdapterTexture;
}

class CrossDeviceTest : public ::testing::Test
{
protected:
    void CreateDevices(SecondDeviceAdapter Adapter)
    {
        GPUTestingEnvironment* pEnv = GPUTestingEnvironment::GetInstance();
        m_pDeviceA                  = pEnv->GetDevice();
        m_pCtxA                     = pEnv->GetDeviceContext();

        const RENDER_DEVICE_TYPE Type = m_pDeviceA->GetDeviceInfo().Type;
        if (Type != RENDER_DEVICE_TYPE_VULKAN && Type != RENDER_DEVICE_TYPE_D3D12)
            GTEST_SKIP() << "Vulkan and Direct3D12 only";

        if (Adapter == SecondDeviceAdapter::Same && Type != RENDER_DEVICE_TYPE_VULKAN)
            GTEST_SKIP() << "A second Direct3D12 device on the same adapter is the same ID3D12Device";

        if (!CreateSecondDevice(m_pDeviceA, Adapter, m_pDeviceB, m_pCtxB))
            GTEST_SKIP() << (Adapter == SecondDeviceAdapter::Other ? "No second hardware adapter" : "Failed to create a second device on the adapter");

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

// Device B on another GPU
class CrossDeviceTransferManagerTest : public CrossDeviceTest
{
protected:
    void SetUp() override
    {
        CreateDevices(SecondDeviceAdapter::Other);
    }
};

// Device B on the GPU of device A (Vulkan only)
class CrossDeviceSameGpuTest : public CrossDeviceTest
{
protected:
    void SetUp() override
    {
        CreateDevices(SecondDeviceAdapter::Same);
    }
};

// Direct3D12 shares fences between adapters (cross-adapter fences). Vulkan does not allow sharing
// semaphores between different GPUs, so the devices fall back to CPU waits.
TEST_F(CrossDeviceTransferManagerTest, SharedFence)
{
    if (m_pDeviceA->GetDeviceInfo().Type == RENDER_DEVICE_TYPE_D3D12)
    {
        TestGpuWaitsForOtherDevice(m_pDeviceA, m_pCtxA, m_pDeviceB, m_pCtxB);
    }
    else
    {
        FenceDesc Desc;
        Desc.Name = "Cross-device fence";
        Desc.Type = FENCE_TYPE_GENERAL;
        RefCntAutoPtr<IFence> pFenceA;
        RefCntAutoPtr<IFence> pFenceB;
        EXPECT_FALSE(CreateCrossDeviceFence(m_pDeviceA, m_pDeviceB, Desc, &pFenceA, &pFenceB))
            << "Vulkan semaphores can only be shared between devices with the same deviceUUID and driverUUID";
        EXPECT_EQ(pFenceA, nullptr);
        EXPECT_EQ(pFenceB, nullptr);
        std::cout << "    Vulkan: the devices are on different GPUs and can not share a semaphore (expected)" << std::endl;
    }
}

// Every frame arrives intact through shared memory, in both directions
TEST_F(CrossDeviceTransferManagerTest, SharedMemory)
{
    const RENDER_DEVICE_TYPE Type         = m_pDeviceA->GetDeviceInfo().Type;
    const TextureDesc        FrameDesc    = GetFrameDesc(256, 128);
    const TransferMode       ExpectedMode = GetExpectedSharedMode(Type);
    constexpr Uint32         NumFrames    = 8;

    IRenderDevice*  pDevices[]  = {m_pDeviceA, m_pDeviceB};
    IDeviceContext* pContexts[] = {m_pCtxA, m_pCtxB};
    for (const bool AllowGpuSync : {true, false})
    {
        for (int Src = 0; Src < 2; ++Src)
        {
            const int Dst = 1 - Src;

            const TransferResult Result = RunTransfer(pDevices[Src], pContexts[Src], pDevices[Dst], pContexts[Dst], FrameDesc, NumFrames,
                                                      /*AllowSharedMemory = */ true, AllowGpuSync, /*VerifyFrames = */ true);
            std::cout << "    " << (Src == 0 ? "A -> B" : "B -> A") << ": " << CrossDeviceTransferManager::GetTransferModeString(Result.Mode)
                      << ", " << GetSyncString(Result.GpuSync) << ", " << Result.NumCorrectFrames << "/" << Result.NumTransfers << " frames correct" << std::endl;

            EXPECT_EQ(Result.Mode, ExpectedMode) << "The devices did not share memory";
            // Direct3D12 adapters share fences; Vulkan devices on different GPUs can not
            EXPECT_EQ(Result.GpuSync, AllowGpuSync && Type == RENDER_DEVICE_TYPE_D3D12);
            EXPECT_EQ(Result.NumTransfers, NumFrames - 1);
            EXPECT_EQ(Result.NumCorrectFrames, Result.NumTransfers);
        }
    }
}

// The CPU copy fallback still works when sharing is not allowed
TEST_F(CrossDeviceTransferManagerTest, CpuCopy)
{
    const TextureDesc FrameDesc = GetFrameDesc(256, 128);
    constexpr Uint32  NumFrames = 8;

    const TransferResult Result = RunTransfer(m_pDeviceA, m_pCtxA, m_pDeviceB, m_pCtxB, FrameDesc, NumFrames,
                                              /*AllowSharedMemory = */ false, /*AllowGpuSync = */ true, /*VerifyFrames = */ true);
    EXPECT_EQ(Result.Mode, TransferMode::CpuCopy);
    EXPECT_FALSE(Result.GpuSync) << "The CPU copies the frames, so it waits for them";
    EXPECT_EQ(Result.NumTransfers, NumFrames - 1);
    EXPECT_EQ(Result.NumCorrectFrames, Result.NumTransfers);
}

void RunTiming(IRenderDevice* pDeviceA, IDeviceContext* pCtxA, IRenderDevice* pDeviceB, IDeviceContext* pCtxB)
{
    const TextureDesc FrameDesc = GetFrameDesc(1920, 1080);
    constexpr Uint32  NumFrames = 60;

    struct Variant
    {
        bool AllowShared;
        bool AllowGpuSync;
    };
    for (const Variant& V : {Variant{true, true}, Variant{true, false}, Variant{false, false}})
    {
        const TransferResult Result = RunTransfer(pDeviceA, pCtxA, pDeviceB, pCtxB, FrameDesc, NumFrames, V.AllowShared, V.AllowGpuSync, /*VerifyFrames = */ false);
        std::cout << "    A -> B, 1920x1080 RGBA8, " << NumFrames << " frames, " << CrossDeviceTransferManager::GetTransferModeString(Result.Mode)
                  << ", " << GetSyncString(Result.GpuSync) << ": " << Result.MillisecondsPerFrm << " ms per frame" << std::endl;
        EXPECT_EQ(Result.NumTransfers, NumFrames - 1);
    }
}

// Informational: time per 1920x1080 RGBA8 frame through shared memory with GPU and CPU waits, and through the CPU
TEST_F(CrossDeviceTransferManagerTest, Timing)
{
    RunTiming(m_pDeviceA, m_pCtxA, m_pDeviceB, m_pCtxB);
}

// Two Vulkan devices on one GPU share a timeline semaphore
TEST_F(CrossDeviceSameGpuTest, SharedFence)
{
    TestGpuWaitsForOtherDevice(m_pDeviceA, m_pCtxA, m_pDeviceB, m_pCtxB);
}

// Frames go through shared host memory, and the devices wait for each other on the GPU
TEST_F(CrossDeviceSameGpuTest, SharedMemoryWithGpuSync)
{
    const TextureDesc FrameDesc = GetFrameDesc(256, 128);
    constexpr Uint32  NumFrames = 8;

    IRenderDevice*  pDevices[]  = {m_pDeviceA, m_pDeviceB};
    IDeviceContext* pContexts[] = {m_pCtxA, m_pCtxB};
    for (int Src = 0; Src < 2; ++Src)
    {
        const int Dst = 1 - Src;

        const TransferResult Result = RunTransfer(pDevices[Src], pContexts[Src], pDevices[Dst], pContexts[Dst], FrameDesc, NumFrames,
                                                  /*AllowSharedMemory = */ true, /*AllowGpuSync = */ true, /*VerifyFrames = */ true);
        std::cout << "    " << (Src == 0 ? "A -> B" : "B -> A") << ": " << CrossDeviceTransferManager::GetTransferModeString(Result.Mode)
                  << ", " << GetSyncString(Result.GpuSync) << ", " << Result.NumCorrectFrames << "/" << Result.NumTransfers << " frames correct" << std::endl;

        EXPECT_EQ(Result.Mode, TransferMode::SharedHostMemory);
        EXPECT_TRUE(Result.GpuSync) << "The devices did not share fences";
        EXPECT_EQ(Result.NumTransfers, NumFrames - 1);
        EXPECT_EQ(Result.NumCorrectFrames, Result.NumTransfers);
    }
}

// Informational
TEST_F(CrossDeviceSameGpuTest, Timing)
{
    RunTiming(m_pDeviceA, m_pCtxA, m_pDeviceB, m_pCtxB);
}

} // namespace
