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

#include "GPUTestingEnvironment.hpp"

#if D3D12_SUPPORTED
#    include <d3d12.h>
#    include "EngineFactoryD3D12.h"
#    include "RenderDeviceD3D12.h"
#    include "BufferD3D12.h"
#    include "TextureD3D12.h"
#endif

#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "gtest/gtest.h"

using namespace Diligent;
using namespace Diligent::Testing;

namespace
{

#if D3D12_SUPPORTED

// A Direct3D12 device on a hardware adapter other than the one of the testing environment
struct OtherAdapter
{
    RefCntAutoPtr<IRenderDevice>      pDevice;
    RefCntAutoPtr<IRenderDeviceD3D12> pDeviceD3D12;
    RefCntAutoPtr<IDeviceContext>     pContext;
    std::string                       Name;
};

bool CreateDeviceOnOtherAdapter(IRenderDevice* pEnvDevice, OtherAdapter& Other)
{
    RefCntAutoPtr<IEngineFactoryD3D12> pFactory{pEnvDevice->GetEngineFactory(), IID_EngineFactoryD3D12};
    RefCntAutoPtr<IRenderDeviceD3D12>  pEnvDeviceD3D12{pEnvDevice, IID_RenderDeviceD3D12};
    if (!pFactory || !pEnvDeviceD3D12)
        return false;

    const LUID EnvLuid = pEnvDeviceD3D12->GetD3D12Device()->GetAdapterLuid();

    const Version MinVersion{11, 0};
    Uint32        NumAdapters = 0;
    pFactory->EnumerateAdapters(MinVersion, NumAdapters, nullptr);
    std::vector<GraphicsAdapterInfo> Adapters(NumAdapters);
    if (NumAdapters > 0)
        pFactory->EnumerateAdapters(MinVersion, NumAdapters, Adapters.data());

    for (Uint32 i = 0; i < NumAdapters; ++i)
    {
        if (Adapters[i].Type == ADAPTER_TYPE_SOFTWARE)
            continue;

        EngineD3D12CreateInfo EngineCI;
        EngineCI.AdapterId          = i;
        EngineCI.GraphicsAPIVersion = MinVersion;

        RefCntAutoPtr<IRenderDevice>  pDevice;
        RefCntAutoPtr<IDeviceContext> pContext;
        pFactory->CreateDeviceAndContextsD3D12(EngineCI, &pDevice, &pContext);
        if (!pDevice || !pContext)
            continue;

        RefCntAutoPtr<IRenderDeviceD3D12> pDeviceD3D12{pDevice, IID_RenderDeviceD3D12};
        const LUID                        Luid = pDeviceD3D12->GetD3D12Device()->GetAdapterLuid();
        if (Luid.LowPart == EnvLuid.LowPart && Luid.HighPart == EnvLuid.HighPart)
            continue;

        Other.pDevice      = std::move(pDevice);
        Other.pDeviceD3D12 = std::move(pDeviceD3D12);
        Other.pContext     = std::move(pContext);
        Other.Name         = Adapters[i].Description;
        return true;
    }
    return false;
}

// Prints debug-layer warnings and errors stored since the last call, returns the number of errors
size_t DrainD3D12Messages(const char* Label, ID3D12Device* pd3d12Device)
{
    ID3D12InfoQueue* pQueue = nullptr;
    if (FAILED(pd3d12Device->QueryInterface(IID_PPV_ARGS(&pQueue))))
    {
        std::cout << "    " << Label << ": no D3D12 info queue (debug layer is off)" << std::endl;
        return 0;
    }

    size_t       NumErrors   = 0;
    const UINT64 NumMessages = pQueue->GetNumStoredMessages();
    for (UINT64 i = 0; i < NumMessages; ++i)
    {
        SIZE_T Length = 0;
        pQueue->GetMessage(i, nullptr, &Length);
        std::vector<Uint8> Storage(Length);
        D3D12_MESSAGE*     pMessage = reinterpret_cast<D3D12_MESSAGE*>(Storage.data());
        if (FAILED(pQueue->GetMessage(i, pMessage, &Length)) || pMessage->Severity > D3D12_MESSAGE_SEVERITY_WARNING)
            continue;
        std::cout << "    " << Label << " D3D12 " << (pMessage->Severity == D3D12_MESSAGE_SEVERITY_WARNING ? "warning: " : "error: ") << pMessage->pDescription << std::endl;
        if (pMessage->Severity < D3D12_MESSAGE_SEVERITY_WARNING)
            ++NumErrors;
    }
    std::cout << "    " << Label << ": " << NumMessages << " stored D3D12 message(s), " << NumErrors << " error(s)" << std::endl;
    pQueue->ClearStoredMessages();
    pQueue->Release();
    return NumErrors;
}

std::vector<Uint8> MakeData(size_t Size, Uint8 Seed)
{
    std::vector<Uint8> Data(Size);
    for (size_t i = 0; i < Data.size(); ++i)
        Data[i] = static_cast<Uint8>(i * 13 + Seed);
    return Data;
}

template <typename ResourceType>
void ReleaseToCommonImpl(IDeviceContext* pContext, ResourceType* pResource)
{
    StateTransitionDesc Barrier{pResource, RESOURCE_STATE_UNKNOWN, RESOURCE_STATE_COMMON, STATE_TRANSITION_FLAG_UPDATE_STATE};
    pContext->TransitionResourceStates(1, &Barrier);
    pContext->WaitForIdle();
}
void ReleaseToCommon(IDeviceContext* pContext, IBuffer* pBuffer) { ReleaseToCommonImpl(pContext, pBuffer); }
void ReleaseToCommon(IDeviceContext* pContext, ITexture* pTexture) { ReleaseToCommonImpl(pContext, pTexture); }

bool BufferContentsEqual(IRenderDevice* pDevice, IDeviceContext* pContext, IBuffer* pSrc, const std::vector<Uint8>& Expected)
{
    BufferDesc StagingDesc;
    StagingDesc.Name           = "Probe staging buffer";
    StagingDesc.Size           = Expected.size();
    StagingDesc.Usage          = USAGE_STAGING;
    StagingDesc.CPUAccessFlags = CPU_ACCESS_READ;

    RefCntAutoPtr<IBuffer> pStaging;
    pDevice->CreateBuffer(StagingDesc, nullptr, &pStaging);
    if (!pStaging)
        return false;

    pContext->CopyBuffer(pSrc, 0, RESOURCE_STATE_TRANSITION_MODE_TRANSITION, pStaging, 0, Expected.size(), RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    pContext->WaitForIdle();

    void* pData = nullptr;
    pContext->MapBuffer(pStaging, MAP_READ, MAP_FLAG_DO_NOT_WAIT, pData);
    const bool Equal = pData != nullptr && std::memcmp(pData, Expected.data(), Expected.size()) == 0;
    pContext->UnmapBuffer(pStaging, MAP_READ);
    return Equal;
}

// Device A (testing environment) and device B (another adapter) write and read one shared buffer
TEST(CrossAdapterSharingProbe, Buffer)
{
    GPUTestingEnvironment* pEnv     = GPUTestingEnvironment::GetInstance();
    IRenderDevice*         pDeviceA = pEnv->GetDevice();
    IDeviceContext*        pCtxA    = pEnv->GetDeviceContext();
    if (pDeviceA->GetDeviceInfo().Type != RENDER_DEVICE_TYPE_D3D12)
        GTEST_SKIP() << "Direct3D12 only";

    GPUTestingEnvironment::ScopedReset EnvironmentAutoReset;

    OtherAdapter B;
    if (!CreateDeviceOnOtherAdapter(pDeviceA, B))
        GTEST_SKIP() << "No second hardware adapter";
    std::cout << "    Device A: " << pDeviceA->GetAdapterInfo().Description << "\n    Device B: " << B.Name << std::endl;

    RefCntAutoPtr<IRenderDeviceD3D12> pDeviceAD3D12{pDeviceA, IID_RenderDeviceD3D12};
    DrainD3D12Messages("A before the probe", pDeviceAD3D12->GetD3D12Device());

    BufferDesc SharedDesc;
    SharedDesc.Name      = "Cross-adapter buffer";
    SharedDesc.Size      = 65536 + 256; // more than one 64 KB page
    SharedDesc.Usage     = USAGE_DEFAULT;
    SharedDesc.BindFlags = BIND_SHADER_RESOURCE | BIND_UNORDERED_ACCESS;
    SharedDesc.Mode      = BUFFER_MODE_RAW;
    SharedDesc.MiscFlags = MISC_BUFFER_FLAG_SHARED_CROSS_ADAPTER;

    const std::vector<Uint8> DataFromA = MakeData(static_cast<size_t>(SharedDesc.Size), 7);
    const std::vector<Uint8> DataFromB = MakeData(static_cast<size_t>(SharedDesc.Size), 101);

    const BufferData       InitA{DataFromA.data(), DataFromA.size()};
    RefCntAutoPtr<IBuffer> pSharedA;
    pDeviceA->CreateBuffer(SharedDesc, &InitA, &pSharedA);
    ASSERT_NE(pSharedA, nullptr);
    ReleaseToCommon(pCtxA, pSharedA);

    RefCntAutoPtr<IBufferD3D12> pSharedAD3D12{pSharedA, IID_BufferD3D12};
    ASSERT_NE(pSharedAD3D12->GetD3D12SharedHeap(), nullptr);

    HANDLE hShared = nullptr;
    ASSERT_TRUE(SUCCEEDED(pDeviceAD3D12->GetD3D12Device()->CreateSharedHandle(pSharedAD3D12->GetD3D12SharedHeap(), nullptr, GENERIC_ALL, nullptr, &hShared)));

    ID3D12Device* pd3d12DeviceB = B.pDeviceD3D12->GetD3D12Device();
    ID3D12Heap*   pHeapB        = nullptr;
    HRESULT       hr            = pd3d12DeviceB->OpenSharedHandle(hShared, IID_PPV_ARGS(&pHeapB));
    CloseHandle(hShared);
    ASSERT_TRUE(SUCCEEDED(hr)) << "OpenSharedHandle failed: 0x" << std::hex << hr;

    Uint64                    Offset   = 0;
    const D3D12_RESOURCE_DESC ResDesc  = pSharedAD3D12->GetD3D12Buffer(Offset, pCtxA)->GetDesc();
    ID3D12Resource*           pPlacedB = nullptr;
    hr                                 = pd3d12DeviceB->CreatePlacedResource(pHeapB, 0, &ResDesc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&pPlacedB));
    pHeapB->Release();
    ASSERT_TRUE(SUCCEEDED(hr)) << "CreatePlacedResource failed: 0x" << std::hex << hr;

    BufferDesc ImportDesc = SharedDesc;
    ImportDesc.Name       = "Cross-adapter buffer opened on B";
    ImportDesc.MiscFlags  = MISC_BUFFER_FLAG_NONE;

    RefCntAutoPtr<IBuffer> pSharedB;
    B.pDeviceD3D12->CreateBufferFromD3DResource(pPlacedB, ImportDesc, RESOURCE_STATE_COMMON, &pSharedB);
    pPlacedB->Release();
    ASSERT_NE(pSharedB, nullptr);

    // A wrote the initial data, B reads it
    EXPECT_TRUE(BufferContentsEqual(B.pDevice, B.pContext, pSharedB, DataFromA)) << "B does not see the data written by A";
    ReleaseToCommon(B.pContext, pSharedB);

    // B writes, A reads
    BufferDesc SrcDesc;
    SrcDesc.Name  = "Source buffer on B";
    SrcDesc.Size  = SharedDesc.Size;
    SrcDesc.Usage = USAGE_IMMUTABLE;
    const BufferData       InitB{DataFromB.data(), DataFromB.size()};
    RefCntAutoPtr<IBuffer> pSrcB;
    B.pDevice->CreateBuffer(SrcDesc, &InitB, &pSrcB);
    ASSERT_NE(pSrcB, nullptr);
    B.pContext->CopyBuffer(pSrcB, 0, RESOURCE_STATE_TRANSITION_MODE_TRANSITION, pSharedB, 0, SharedDesc.Size, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    ReleaseToCommon(B.pContext, pSharedB);

    pSharedA->SetState(RESOURCE_STATE_COMMON);
    EXPECT_TRUE(BufferContentsEqual(pDeviceA, pCtxA, pSharedA, DataFromB)) << "A does not see the data written by B";

    EXPECT_EQ(DrainD3D12Messages("A", pDeviceAD3D12->GetD3D12Device()), size_t{0});
    EXPECT_EQ(DrainD3D12Messages("B", B.pDeviceD3D12->GetD3D12Device()), size_t{0});
}

bool TextureContentsEqual(IRenderDevice* pDevice, IDeviceContext* pContext, ITexture* pSrc, const std::vector<Uint8>& Expected)
{
    TextureDesc StagingDesc    = pSrc->GetDesc();
    StagingDesc.Name           = "Probe staging texture";
    StagingDesc.BindFlags      = BIND_NONE;
    StagingDesc.MiscFlags      = MISC_TEXTURE_FLAG_NONE;
    StagingDesc.Usage          = USAGE_STAGING;
    StagingDesc.CPUAccessFlags = CPU_ACCESS_READ;

    RefCntAutoPtr<ITexture> pStaging;
    pDevice->CreateTexture(StagingDesc, nullptr, &pStaging);
    if (!pStaging)
        return false;

    CopyTextureAttribs Copy{pSrc, RESOURCE_STATE_TRANSITION_MODE_TRANSITION, pStaging, RESOURCE_STATE_TRANSITION_MODE_TRANSITION};
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

RefCntAutoPtr<ITexture> CreateFilledTexture(IRenderDevice* pDevice, const TextureDesc& Desc, const std::vector<Uint8>& Data)
{
    TextureDesc SrcDesc = Desc;
    SrcDesc.Name        = "Probe source texture";
    SrcDesc.MiscFlags   = MISC_TEXTURE_FLAG_NONE;
    SrcDesc.BindFlags   = BIND_SHADER_RESOURCE;
    SrcDesc.Usage       = USAGE_IMMUTABLE;

    TextureSubResData Sub{Data.data(), Uint64{Desc.Width} * 4};
    TextureData       Init{&Sub, 1};

    RefCntAutoPtr<ITexture> pTex;
    pDevice->CreateTexture(SrcDesc, &Init, &pTex);
    return pTex;
}

bool SupportsCrossAdapterRowMajor(ID3D12Device* pd3d12Device)
{
    D3D12_FEATURE_DATA_D3D12_OPTIONS Options{};
    return SUCCEEDED(pd3d12Device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &Options, sizeof(Options))) && Options.CrossAdapterRowMajorTextureSupported;
}

// Device A (testing environment) and device B (another adapter) write and read one shared texture
TEST(CrossAdapterSharingProbe, Texture)
{
    GPUTestingEnvironment* pEnv     = GPUTestingEnvironment::GetInstance();
    IRenderDevice*         pDeviceA = pEnv->GetDevice();
    IDeviceContext*        pCtxA    = pEnv->GetDeviceContext();
    if (pDeviceA->GetDeviceInfo().Type != RENDER_DEVICE_TYPE_D3D12)
        GTEST_SKIP() << "Direct3D12 only";

    GPUTestingEnvironment::ScopedReset EnvironmentAutoReset;

    OtherAdapter B;
    if (!CreateDeviceOnOtherAdapter(pDeviceA, B))
        GTEST_SKIP() << "No second hardware adapter";

    RefCntAutoPtr<IRenderDeviceD3D12> pDeviceAD3D12{pDeviceA, IID_RenderDeviceD3D12};
    const bool                        RowMajorA = SupportsCrossAdapterRowMajor(pDeviceAD3D12->GetD3D12Device());
    const bool                        RowMajorB = SupportsCrossAdapterRowMajor(B.pDeviceD3D12->GetD3D12Device());
    std::cout << "    Device A: " << pDeviceA->GetAdapterInfo().Description << ", CrossAdapterRowMajorTextureSupported=" << RowMajorA
              << "\n    Device B: " << B.Name << ", CrossAdapterRowMajorTextureSupported=" << RowMajorB << std::endl;
    if (!RowMajorA || !RowMajorB)
        GTEST_SKIP() << "Cross-adapter row-major textures are not supported on both adapters";

    TextureDesc SharedDesc;
    SharedDesc.Name      = "Cross-adapter texture";
    SharedDesc.Type      = RESOURCE_DIM_TEX_2D;
    SharedDesc.Width     = 64;
    SharedDesc.Height    = 32;
    SharedDesc.Format    = TEX_FORMAT_RGBA8_UNORM;
    SharedDesc.Usage     = USAGE_DEFAULT;
    SharedDesc.MiscFlags = MISC_TEXTURE_FLAG_SHARED_CROSS_ADAPTER;

    const size_t             Size      = size_t{SharedDesc.Width} * SharedDesc.Height * 4;
    const std::vector<Uint8> DataFromA = MakeData(Size, 7);
    const std::vector<Uint8> DataFromB = MakeData(Size, 101);

    RefCntAutoPtr<ITexture> pSharedA;
    pDeviceA->CreateTexture(SharedDesc, nullptr, &pSharedA);
    ASSERT_NE(pSharedA, nullptr);

    RefCntAutoPtr<ITexture> pSrcA = CreateFilledTexture(pDeviceA, SharedDesc, DataFromA);
    ASSERT_NE(pSrcA, nullptr);
    {
        CopyTextureAttribs Copy{pSrcA, RESOURCE_STATE_TRANSITION_MODE_TRANSITION, pSharedA, RESOURCE_STATE_TRANSITION_MODE_TRANSITION};
        pCtxA->CopyTexture(Copy);
    }
    ReleaseToCommon(pCtxA, pSharedA);

    RefCntAutoPtr<ITextureD3D12> pSharedAD3D12{pSharedA, IID_TextureD3D12};
    HANDLE                       hShared = nullptr;
    ASSERT_TRUE(SUCCEEDED(pDeviceAD3D12->GetD3D12Device()->CreateSharedHandle(pSharedAD3D12->GetD3D12Texture(), nullptr, GENERIC_ALL, nullptr, &hShared)));

    ID3D12Resource* pOpenedB = nullptr;
    const HRESULT   hr       = B.pDeviceD3D12->GetD3D12Device()->OpenSharedHandle(hShared, IID_PPV_ARGS(&pOpenedB));
    CloseHandle(hShared);
    ASSERT_TRUE(SUCCEEDED(hr)) << "OpenSharedHandle failed: 0x" << std::hex << hr;

    RefCntAutoPtr<ITexture> pSharedB;
    B.pDeviceD3D12->CreateTextureFromD3DResource(pOpenedB, RESOURCE_STATE_COMMON, &pSharedB);
    pOpenedB->Release();
    ASSERT_NE(pSharedB, nullptr);

    // A wrote, B reads
    EXPECT_TRUE(TextureContentsEqual(B.pDevice, B.pContext, pSharedB, DataFromA)) << "B does not see the data written by A";
    ReleaseToCommon(B.pContext, pSharedB);

    // B writes, A reads
    RefCntAutoPtr<ITexture> pSrcB = CreateFilledTexture(B.pDevice, SharedDesc, DataFromB);
    ASSERT_NE(pSrcB, nullptr);
    {
        CopyTextureAttribs Copy{pSrcB, RESOURCE_STATE_TRANSITION_MODE_TRANSITION, pSharedB, RESOURCE_STATE_TRANSITION_MODE_TRANSITION};
        B.pContext->CopyTexture(Copy);
    }
    ReleaseToCommon(B.pContext, pSharedB);

    pSharedA->SetState(RESOURCE_STATE_COMMON);
    EXPECT_TRUE(TextureContentsEqual(pDeviceA, pCtxA, pSharedA, DataFromB)) << "A does not see the data written by B";

    EXPECT_EQ(DrainD3D12Messages("A", pDeviceAD3D12->GetD3D12Device()), size_t{0});
    EXPECT_EQ(DrainD3D12Messages("B", B.pDeviceD3D12->GetD3D12Device()), size_t{0});
}

#endif // D3D12_SUPPORTED

} // namespace
