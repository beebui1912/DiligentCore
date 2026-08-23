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
#    include "RenderDeviceD3D12.h"
#    include "TextureD3D12.h"
#    include "BufferD3D12.h"
#endif

#include <cstring>
#include <vector>

#include "gtest/gtest.h"

using namespace Diligent;
using namespace Diligent::Testing;

namespace
{

TextureDesc GetSharedTextureDesc()
{
    TextureDesc Desc;
    Desc.Name      = "Shared cross-adapter texture";
    Desc.Type      = RESOURCE_DIM_TEX_2D;
    Desc.Width     = 16;
    Desc.Height    = 16;
    Desc.Format    = TEX_FORMAT_RGBA8_UNORM;
    Desc.Usage     = USAGE_DEFAULT;
    Desc.MiscFlags = MISC_TEXTURE_FLAG_SHARED_CROSS_ADAPTER;
    return Desc;
}

// Backends other than Direct3D12 reject the flag; every backend rejects a texture Direct3D12 can not share
TEST(SharedCrossAdapterTexture, InvalidDesc)
{
    GPUTestingEnvironment* pEnv    = GPUTestingEnvironment::GetInstance();
    IRenderDevice*         pDevice = pEnv->GetDevice();

    GPUTestingEnvironment::ScopedReset EnvironmentAutoReset;

    TextureDesc Desc = GetSharedTextureDesc();
    if (pDevice->GetDeviceInfo().Type != RENDER_DEVICE_TYPE_D3D12)
    {
        TestingEnvironment::ErrorScope ExpectedErrors{"Failed to create Texture", "is only supported in Direct3D12"};
        RefCntAutoPtr<ITexture>        pTexture;
        pDevice->CreateTexture(Desc, nullptr, &pTexture);
        EXPECT_EQ(pTexture, nullptr);
        return;
    }

    {
        Desc.MipLevels = 2;
        TestingEnvironment::ErrorScope ExpectedErrors{"Failed to create Texture", "requires one mip level"};
        RefCntAutoPtr<ITexture>        pTexture;
        pDevice->CreateTexture(Desc, nullptr, &pTexture);
        EXPECT_EQ(pTexture, nullptr);
        Desc.MipLevels = 1;
    }
    {
        Desc.BindFlags = BIND_DEPTH_STENCIL;
        Desc.Format    = TEX_FORMAT_D32_FLOAT;
        TestingEnvironment::ErrorScope ExpectedErrors{"Failed to create Texture", "not compatible with BIND_DEPTH_STENCIL"};
        RefCntAutoPtr<ITexture>        pTexture;
        pDevice->CreateTexture(Desc, nullptr, &pTexture);
        EXPECT_EQ(pTexture, nullptr);
    }
}

BufferDesc GetSharedBufferDesc()
{
    BufferDesc Desc;
    Desc.Name      = "Shared cross-adapter buffer";
    Desc.Size      = 4096;
    Desc.Usage     = USAGE_DEFAULT;
    Desc.BindFlags = BIND_SHADER_RESOURCE | BIND_UNORDERED_ACCESS;
    Desc.Mode      = BUFFER_MODE_RAW;
    Desc.MiscFlags = MISC_BUFFER_FLAG_SHARED_CROSS_ADAPTER;
    return Desc;
}

TEST(SharedCrossAdapterBuffer, InvalidDesc)
{
    GPUTestingEnvironment* pEnv    = GPUTestingEnvironment::GetInstance();
    IRenderDevice*         pDevice = pEnv->GetDevice();

    GPUTestingEnvironment::ScopedReset EnvironmentAutoReset;

    BufferDesc Desc = GetSharedBufferDesc();
    if (pDevice->GetDeviceInfo().Type != RENDER_DEVICE_TYPE_D3D12)
    {
        TestingEnvironment::ErrorScope ExpectedErrors{"Failed to create Buffer", "is only supported in Direct3D12"};
        RefCntAutoPtr<IBuffer>         pBuffer;
        pDevice->CreateBuffer(Desc, nullptr, &pBuffer);
        EXPECT_EQ(pBuffer, nullptr);
        return;
    }

    Desc.Usage          = USAGE_STAGING;
    Desc.BindFlags      = BIND_NONE;
    Desc.CPUAccessFlags = CPU_ACCESS_READ;
    TestingEnvironment::ErrorScope ExpectedErrors{"Failed to create Buffer", "requires USAGE_DEFAULT"};
    RefCntAutoPtr<IBuffer>         pBuffer;
    pDevice->CreateBuffer(Desc, nullptr, &pBuffer);
    EXPECT_EQ(pBuffer, nullptr);
}

#if D3D12_SUPPORTED
// An ordinary buffer has no shared heap
TEST(SharedCrossAdapterBuffer, NoHeapWithoutFlag)
{
    GPUTestingEnvironment* pEnv    = GPUTestingEnvironment::GetInstance();
    IRenderDevice*         pDevice = pEnv->GetDevice();
    if (pDevice->GetDeviceInfo().Type != RENDER_DEVICE_TYPE_D3D12)
        GTEST_SKIP() << "Direct3D12 only";

    GPUTestingEnvironment::ScopedReset EnvironmentAutoReset;

    BufferDesc Desc = GetSharedBufferDesc();
    Desc.MiscFlags  = MISC_BUFFER_FLAG_NONE;

    RefCntAutoPtr<IBuffer> pBuffer;
    pDevice->CreateBuffer(Desc, nullptr, &pBuffer);
    ASSERT_NE(pBuffer, nullptr);

    RefCntAutoPtr<IBufferD3D12> pBufferD3D12{pBuffer, IID_BufferD3D12};
    ASSERT_NE(pBufferD3D12, nullptr);
    EXPECT_EQ(pBufferD3D12->GetD3D12SharedHeap(), nullptr);
}

// The sharing path on one device: the handle is the same for a device on another adapter
TEST(SharedCrossAdapterBuffer, CopyThroughSharedHandle)
{
    GPUTestingEnvironment* pEnv     = GPUTestingEnvironment::GetInstance();
    IRenderDevice*         pDevice  = pEnv->GetDevice();
    IDeviceContext*        pContext = pEnv->GetDeviceContext();
    if (pDevice->GetDeviceInfo().Type != RENDER_DEVICE_TYPE_D3D12)
        GTEST_SKIP() << "Direct3D12 only";

    GPUTestingEnvironment::ScopedReset EnvironmentAutoReset;

    RefCntAutoPtr<IRenderDeviceD3D12> pDeviceD3D12{pDevice, IID_RenderDeviceD3D12};
    ASSERT_NE(pDeviceD3D12, nullptr);
    ID3D12Device* pd3d12Device = pDeviceD3D12->GetD3D12Device();

    const BufferDesc SharedDesc = GetSharedBufferDesc();

    const auto MakeData = [&SharedDesc](Uint8 Seed) {
        std::vector<Uint8> Data(static_cast<size_t>(SharedDesc.Size));
        for (size_t i = 0; i < Data.size(); ++i)
            Data[i] = static_cast<Uint8>(i * 13 + Seed);
        return Data;
    };
    const std::vector<Uint8> InitialData = MakeData(7);
    const std::vector<Uint8> CopiedData  = MakeData(101);

    // Initial data goes through the same creation path as an empty shared buffer
    const BufferData       SharedInit{InitialData.data(), InitialData.size()};
    RefCntAutoPtr<IBuffer> pShared;
    pDevice->CreateBuffer(SharedDesc, &SharedInit, &pShared);
    ASSERT_NE(pShared, nullptr);

    RefCntAutoPtr<IBufferD3D12> pSharedD3D12{pShared, IID_BufferD3D12};
    ASSERT_NE(pSharedD3D12, nullptr);

    Uint64          DataStartByteOffset = 0;
    ID3D12Resource* pd3d12Shared        = pSharedD3D12->GetD3D12Buffer(DataStartByteOffset, pContext);
    ASSERT_NE(pd3d12Shared, nullptr);
    EXPECT_NE(pd3d12Shared->GetDesc().Flags & D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER, D3D12_RESOURCE_FLAG_NONE);

    ASSERT_NE(pSharedD3D12->GetD3D12SharedHeap(), nullptr);

    HANDLE hShared = nullptr;
    ASSERT_TRUE(SUCCEEDED(pd3d12Device->CreateSharedHandle(pSharedD3D12->GetD3D12SharedHeap(), nullptr, GENERIC_ALL, nullptr, &hShared)));

    ID3D12Heap* pOpenedHeap = nullptr;
    HRESULT     hr          = pd3d12Device->OpenSharedHandle(hShared, IID_PPV_ARGS(&pOpenedHeap));
    CloseHandle(hShared);
    ASSERT_TRUE(SUCCEEDED(hr));

    const D3D12_RESOURCE_DESC d3d12SharedDesc = pd3d12Shared->GetDesc();
    ID3D12Resource*           pOpened         = nullptr;
    hr                                        = pd3d12Device->CreatePlacedResource(pOpenedHeap, 0, &d3d12SharedDesc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&pOpened));
    pOpenedHeap->Release();
    ASSERT_TRUE(SUCCEEDED(hr));

    BufferDesc ImportDesc = SharedDesc;
    ImportDesc.Name       = "Imported shared buffer";
    ImportDesc.MiscFlags  = MISC_BUFFER_FLAG_NONE;

    RefCntAutoPtr<IBuffer> pImported;
    pDeviceD3D12->CreateBufferFromD3DResource(pOpened, ImportDesc, RESOURCE_STATE_COMMON, &pImported);
    pOpened->Release();
    ASSERT_NE(pImported, nullptr);

    BufferDesc StagingDesc;
    StagingDesc.Name           = "Staging buffer";
    StagingDesc.Size           = SharedDesc.Size;
    StagingDesc.Usage          = USAGE_STAGING;
    StagingDesc.CPUAccessFlags = CPU_ACCESS_READ;

    RefCntAutoPtr<IBuffer> pStaging;
    pDevice->CreateBuffer(StagingDesc, nullptr, &pStaging);
    ASSERT_NE(pStaging, nullptr);

    const auto ExpectContents = [&](IBuffer* pSrc, const std::vector<Uint8>& Expected) {
        pContext->CopyBuffer(pSrc, 0, RESOURCE_STATE_TRANSITION_MODE_TRANSITION, pStaging, 0, SharedDesc.Size, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        pContext->WaitForIdle();

        void* pData = nullptr;
        pContext->MapBuffer(pStaging, MAP_READ, MAP_FLAG_DO_NOT_WAIT, pData);
        ASSERT_NE(pData, nullptr);
        EXPECT_EQ(std::memcmp(pData, Expected.data(), Expected.size()), 0);
        pContext->UnmapBuffer(pStaging, MAP_READ);
    };
    const auto ReleaseToCommon = [&](IBuffer* pBuffer) {
        StateTransitionDesc Barrier{pBuffer, RESOURCE_STATE_UNKNOWN, RESOURCE_STATE_COMMON, STATE_TRANSITION_FLAG_UPDATE_STATE};
        pContext->TransitionResourceStates(1, &Barrier);
        pContext->WaitForIdle();
    };

    // Created with data through the buffer, read through the opened handle
    ReleaseToCommon(pShared);
    ExpectContents(pImported, InitialData);
    ReleaseToCommon(pImported);

    // Written through the opened handle, read through the buffer
    BufferDesc SrcDesc;
    SrcDesc.Name  = "Source buffer";
    SrcDesc.Size  = SharedDesc.Size;
    SrcDesc.Usage = USAGE_IMMUTABLE;
    const BufferData       SrcInit{CopiedData.data(), CopiedData.size()};
    RefCntAutoPtr<IBuffer> pSrc;
    pDevice->CreateBuffer(SrcDesc, &SrcInit, &pSrc);
    ASSERT_NE(pSrc, nullptr);

    pContext->CopyBuffer(pSrc, 0, RESOURCE_STATE_TRANSITION_MODE_TRANSITION, pImported, 0, SharedDesc.Size, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    ReleaseToCommon(pImported);

    pShared->SetState(RESOURCE_STATE_COMMON);
    ExpectContents(pShared, CopiedData);
}

// The sharing path on one device: the handle is the same for a device on another adapter
TEST(SharedCrossAdapterTexture, CopyThroughSharedHandle)
{
    GPUTestingEnvironment* pEnv     = GPUTestingEnvironment::GetInstance();
    IRenderDevice*         pDevice  = pEnv->GetDevice();
    IDeviceContext*        pContext = pEnv->GetDeviceContext();
    if (pDevice->GetDeviceInfo().Type != RENDER_DEVICE_TYPE_D3D12)
        GTEST_SKIP() << "Direct3D12 only";

    GPUTestingEnvironment::ScopedReset EnvironmentAutoReset;

    RefCntAutoPtr<IRenderDeviceD3D12> pDeviceD3D12{pDevice, IID_RenderDeviceD3D12};
    ASSERT_NE(pDeviceD3D12, nullptr);
    ID3D12Device* pd3d12Device = pDeviceD3D12->GetD3D12Device();

    D3D12_FEATURE_DATA_D3D12_OPTIONS Options{};
    if (FAILED(pd3d12Device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &Options, sizeof(Options))) || !Options.CrossAdapterRowMajorTextureSupported)
        GTEST_SKIP() << "Cross-adapter row-major textures are not supported";

    const TextureDesc SharedDesc = GetSharedTextureDesc();

    RefCntAutoPtr<ITexture> pShared;
    pDevice->CreateTexture(SharedDesc, nullptr, &pShared);
    ASSERT_NE(pShared, nullptr);

    RefCntAutoPtr<ITextureD3D12> pSharedD3D12{pShared, IID_TextureD3D12};
    ASSERT_NE(pSharedD3D12, nullptr);

    D3D12_RESOURCE_DESC d3d12Desc = pSharedD3D12->GetD3D12Texture()->GetDesc();
    EXPECT_EQ(d3d12Desc.Layout, D3D12_TEXTURE_LAYOUT_ROW_MAJOR);
    EXPECT_NE(d3d12Desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER, D3D12_RESOURCE_FLAG_NONE);

    HANDLE hShared = nullptr;
    ASSERT_TRUE(SUCCEEDED(pd3d12Device->CreateSharedHandle(pSharedD3D12->GetD3D12Texture(), nullptr, GENERIC_ALL, nullptr, &hShared)));

    ID3D12Resource* pOpened = nullptr;
    const HRESULT   hr      = pd3d12Device->OpenSharedHandle(hShared, IID_PPV_ARGS(&pOpened));
    CloseHandle(hShared);
    ASSERT_TRUE(SUCCEEDED(hr));

    RefCntAutoPtr<ITexture> pImported;
    pDeviceD3D12->CreateTextureFromD3DResource(pOpened, RESOURCE_STATE_COMMON, &pImported);
    pOpened->Release();
    ASSERT_NE(pImported, nullptr);

    // Write through the texture, read through the opened handle
    std::vector<Uint8> Data(size_t{SharedDesc.Width} * SharedDesc.Height * 4);
    for (size_t i = 0; i < Data.size(); ++i)
        Data[i] = static_cast<Uint8>(i * 13 + 7);

    TextureDesc SrcDesc   = SharedDesc;
    SrcDesc.Name          = "Source texture";
    SrcDesc.MiscFlags     = MISC_TEXTURE_FLAG_NONE;
    SrcDesc.BindFlags     = BIND_SHADER_RESOURCE;
    TextureSubResData Sub{Data.data(), Uint64{SharedDesc.Width} * 4};
    TextureData       InitData{&Sub, 1};

    RefCntAutoPtr<ITexture> pSrc;
    pDevice->CreateTexture(SrcDesc, &InitData, &pSrc);
    ASSERT_NE(pSrc, nullptr);

    TextureDesc StagingDesc    = SharedDesc;
    StagingDesc.Name           = "Staging texture";
    StagingDesc.MiscFlags      = MISC_TEXTURE_FLAG_NONE;
    StagingDesc.Usage          = USAGE_STAGING;
    StagingDesc.CPUAccessFlags = CPU_ACCESS_READ;

    RefCntAutoPtr<ITexture> pStaging;
    pDevice->CreateTexture(StagingDesc, nullptr, &pStaging);
    ASSERT_NE(pStaging, nullptr);

    {
        CopyTextureAttribs Copy{pSrc, RESOURCE_STATE_TRANSITION_MODE_TRANSITION, pShared, RESOURCE_STATE_TRANSITION_MODE_TRANSITION};
        pContext->CopyTexture(Copy);

        StateTransitionDesc Barrier{pShared, RESOURCE_STATE_UNKNOWN, RESOURCE_STATE_COMMON, STATE_TRANSITION_FLAG_UPDATE_STATE};
        pContext->TransitionResourceStates(1, &Barrier);
        pContext->WaitForIdle();
    }
    {
        CopyTextureAttribs Copy{pImported, RESOURCE_STATE_TRANSITION_MODE_TRANSITION, pStaging, RESOURCE_STATE_TRANSITION_MODE_TRANSITION};
        pContext->CopyTexture(Copy);
        pContext->WaitForIdle();
    }

    MappedTextureSubresource Mapped;
    pContext->MapTextureSubresource(pStaging, 0, 0, MAP_READ, MAP_FLAG_DO_NOT_WAIT, nullptr, Mapped);
    ASSERT_NE(Mapped.pData, nullptr);
    bool Match = true;
    for (Uint32 Row = 0; Row < SharedDesc.Height && Match; ++Row)
    {
        Match = std::memcmp(&Data[size_t{Row} * SharedDesc.Width * 4], static_cast<const Uint8*>(Mapped.pData) + Row * Mapped.Stride, size_t{SharedDesc.Width} * 4) == 0;
    }
    pContext->UnmapTextureSubresource(pStaging, 0, 0);
    EXPECT_TRUE(Match);
}

#endif // D3D12_SUPPORTED

} // namespace
