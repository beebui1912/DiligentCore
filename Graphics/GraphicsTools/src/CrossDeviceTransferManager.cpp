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

#include "CrossDeviceTransferManager.hpp"
#include <algorithm>
#include <cstddef>
#include <cstring>
#include "../../../Platforms/Basic/interface/DebugUtilities.hpp"
#include "../../../Primitives/interface/AlignedMalloc.h"
#include "../../../Common/interface/ObjectBase.hpp"
#include "../../GraphicsAccessories/interface/GraphicsAccessories.hpp"

#if VULKAN_SUPPORTED
#    include "../../GraphicsEngineVulkan/include/VulkanUtilities/VulkanHeaders.h"
#    include "RenderDeviceVk.h"
#endif

#if D3D12_SUPPORTED
#    include "WinHPreface.h"
#    include <d3d12.h>
#    include <atlbase.h>
#    include "WinHPostface.h"

#    include "RenderDeviceD3D12.h"
#    include "TextureD3D12.h"
#endif

namespace Diligent
{

namespace
{

#if VULKAN_SUPPORTED

// Host allocation that two Vulkan devices import. Every texture in the allocation keeps a reference,
// so it is freed after both devices have freed their memory objects.
class SharedHostAllocation final : public ObjectBase<IObject>
{
public:
    SharedHostAllocation(IReferenceCounters* pRefCounters, size_t Size, size_t Alignment) :
        ObjectBase<IObject>{pRefCounters},
        m_pData{DILIGENT_ALIGNED_MALLOC(Size, Alignment, __FILE__, __LINE__)}
    {
        // The source GPU writes every byte that the destination GPU reads, but clear the
        // allocation so that no stale process memory is ever exposed to a device.
        if (m_pData != nullptr)
            std::memset(m_pData, 0, Size);
    }

    ~SharedHostAllocation()
    {
        if (m_pData != nullptr)
            DILIGENT_ALIGNED_FREE(m_pData);
    }

    void* GetData() const { return m_pData; }

private:
    void* const m_pData;
};

bool CreateSharedHostMemoryTextures(IRenderDevice*                                         pSrcDevice,
                                    IRenderDevice*                                         pDstDevice,
                                    const TextureDesc&                                     Desc,
                                    std::vector<CrossDeviceTransferManager::FrameBufferTransfer>& Buffers)
{
    RefCntAutoPtr<IRenderDeviceVk> pSrcDeviceVk{pSrcDevice, IID_RenderDeviceVk};
    RefCntAutoPtr<IRenderDeviceVk> pDstDeviceVk{pDstDevice, IID_RenderDeviceVk};
    if (!pSrcDeviceVk || !pDstDeviceVk)
        return false;

    TextureDesc ReadbackDesc    = Desc;
    ReadbackDesc.Name           = "CrossDevice Shared Host Memory (Src)";
    ReadbackDesc.BindFlags      = BIND_NONE;
    ReadbackDesc.MiscFlags      = MISC_TEXTURE_FLAG_NONE;
    ReadbackDesc.Usage          = USAGE_STAGING;
    ReadbackDesc.CPUAccessFlags = CPU_ACCESS_READ;

    TextureDesc UploadDesc    = ReadbackDesc;
    UploadDesc.Name           = "CrossDevice Shared Host Memory (Dst)";
    UploadDesc.CPUAccessFlags = CPU_ACCESS_WRITE;

    Uint64 SrcSize = 0, SrcAlignment = 0;
    Uint64 DstSize = 0, DstAlignment = 0;
    if (!pSrcDeviceVk->GetHostMemoryTextureRequirements(ReadbackDesc, SrcSize, SrcAlignment) ||
        !pDstDeviceVk->GetHostMemoryTextureRequirements(UploadDesc, DstSize, DstAlignment))
        return false;

    // One allocation must satisfy both devices. Alignments are powers of two.
    const size_t Size      = static_cast<size_t>(std::max(SrcSize, DstSize));
    const size_t Alignment = static_cast<size_t>(std::max({SrcAlignment, DstAlignment, Uint64{alignof(std::max_align_t)}}));

    for (CrossDeviceTransferManager::FrameBufferTransfer& Buffer : Buffers)
    {
        RefCntAutoPtr<SharedHostAllocation> pAllocation{MakeNewRCObj<SharedHostAllocation>()(Size, Alignment)};
        if (pAllocation->GetData() == nullptr)
            return false;

        pSrcDeviceVk->CreateTextureFromHostMemory(ReadbackDesc, pAllocation->GetData(), pAllocation, &Buffer.pSecondaryStagingReadback);
        pDstDeviceVk->CreateTextureFromHostMemory(UploadDesc, pAllocation->GetData(), pAllocation, &Buffer.pPrimaryStagingUpload);
        if (!Buffer.pSecondaryStagingReadback || !Buffer.pPrimaryStagingUpload)
            return false;
    }
    return true;
}

#endif // VULKAN_SUPPORTED

#if D3D12_SUPPORTED

bool SupportsCrossAdapterRowMajorTextures(ID3D12Device* pd3d12Device)
{
    D3D12_FEATURE_DATA_D3D12_OPTIONS Options{};
    return SUCCEEDED(pd3d12Device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &Options, sizeof(Options))) &&
        Options.CrossAdapterRowMajorTextureSupported;
}

bool CreateSharedCrossAdapterTextures(IRenderDevice*                                                pSrcDevice,
                                      IRenderDevice*                                                pDstDevice,
                                      const TextureDesc&                                            Desc,
                                      std::vector<CrossDeviceTransferManager::FrameBufferTransfer>& Buffers)
{
    RefCntAutoPtr<IRenderDeviceD3D12> pSrcDeviceD3D12{pSrcDevice, IID_RenderDeviceD3D12};
    RefCntAutoPtr<IRenderDeviceD3D12> pDstDeviceD3D12{pDstDevice, IID_RenderDeviceD3D12};
    if (!pSrcDeviceD3D12 || !pDstDeviceD3D12)
        return false;

    // The requirements of MISC_TEXTURE_FLAG_SHARED_CROSS_ADAPTER (see ValidateTextureDesc())
    const TextureFormatAttribs& FmtAttribs = GetTextureFormatAttribs(Desc.Format);
    if (Desc.Type != RESOURCE_DIM_TEX_2D || Desc.SampleCount > 1 ||
        FmtAttribs.ComponentType == COMPONENT_TYPE_DEPTH || FmtAttribs.ComponentType == COMPONENT_TYPE_DEPTH_STENCIL)
        return false;

    ID3D12Device* pd3d12SrcDevice = pSrcDeviceD3D12->GetD3D12Device();
    ID3D12Device* pd3d12DstDevice = pDstDeviceD3D12->GetD3D12Device();
    if (!SupportsCrossAdapterRowMajorTextures(pd3d12SrcDevice) || !SupportsCrossAdapterRowMajorTextures(pd3d12DstDevice))
        return false;

    TextureDesc SharedDesc    = Desc;
    SharedDesc.Name           = "CrossDevice Shared Cross-Adapter Texture";
    SharedDesc.BindFlags      = BIND_NONE;
    SharedDesc.Usage          = USAGE_DEFAULT;
    SharedDesc.CPUAccessFlags = CPU_ACCESS_NONE;
    SharedDesc.MiscFlags      = MISC_TEXTURE_FLAG_SHARED_CROSS_ADAPTER;

    for (CrossDeviceTransferManager::FrameBufferTransfer& Buffer : Buffers)
    {
        pSrcDevice->CreateTexture(SharedDesc, nullptr, &Buffer.pSecondaryStagingReadback);
        if (!Buffer.pSecondaryStagingReadback)
            return false;

        RefCntAutoPtr<ITextureD3D12> pSrcTexD3D12{Buffer.pSecondaryStagingReadback, IID_TextureD3D12};

        HANDLE hShared = nullptr;
        if (FAILED(pd3d12SrcDevice->CreateSharedHandle(pSrcTexD3D12->GetD3D12Texture(), nullptr, GENERIC_ALL, nullptr, &hShared)))
            return false;

        CComPtr<ID3D12Resource> pd3d12Opened;
        const HRESULT           hr = pd3d12DstDevice->OpenSharedHandle(hShared, IID_PPV_ARGS(&pd3d12Opened));
        CloseHandle(hShared);
        if (FAILED(hr))
            return false;

        pDstDeviceD3D12->CreateTextureFromD3DResource(pd3d12Opened, RESOURCE_STATE_COMMON, &Buffer.pPrimaryStagingUpload);
        if (!Buffer.pPrimaryStagingUpload)
            return false;
    }
    return true;
}

#endif // D3D12_SUPPORTED

// A cross-adapter resource is accessed by the other adapter in the COMMON state
void TransitionToCommon(IDeviceContext* pContext, ITexture* pTexture)
{
    StateTransitionDesc Barrier{pTexture, RESOURCE_STATE_UNKNOWN, RESOURCE_STATE_COMMON, STATE_TRANSITION_FLAG_UPDATE_STATE};
    pContext->TransitionResourceStates(1, &Barrier);
}

} // namespace

const char* CrossDeviceTransferManager::GetTransferModeString(TransferMode Mode)
{
    switch (Mode)
    {
        case TransferMode::None: return "None";
        case TransferMode::CpuCopy: return "CPU copy";
        case TransferMode::SharedHostMemory: return "shared host memory (GPU copies, no CPU copy)";
        case TransferMode::SharedCrossAdapterTexture: return "shared cross-adapter texture (GPU copies, no CPU copy)";
        default:
            UNEXPECTED("Unexpected transfer mode");
            return "Unknown";
    }
}

void CrossDeviceTransferManager::Init(IRenderDevice*     pSrcDevice,
                                      IRenderDevice*     pDstDevice,
                                      const TextureDesc& Desc,
                                      Uint32             FrameLatency,
                                      bool               AllowSharedMemory)
{
    std::lock_guard<std::mutex> Lock{m_Mtx};

    Reset();

    m_pSrcDevice = pSrcDevice;
    m_pDstDevice = pDstDevice;
    m_TexDesc    = Desc;
    m_Latency    = (std::max)(1u, FrameLatency);
    m_Buffers.resize(m_Latency);

    // The transfer memory holds one subresource
    TextureDesc TransferDesc = Desc;
    TransferDesc.MipLevels   = 1;
    TransferDesc.ArraySize   = 1;

    FenceDesc fenceDesc;
    fenceDesc.Name = "CrossDevice Transfer Fence";

    for (Uint32 i = 0; i < m_Latency; ++i)
    {
        if (m_pSrcDevice)
            m_pSrcDevice->CreateFence(fenceDesc, &m_Buffers[i].pSecondaryFence);
        if (m_pDstDevice)
            m_pDstDevice->CreateFence(fenceDesc, &m_Buffers[i].pPrimaryFence);
    }

    m_Mode = TransferMode::CpuCopy;
    if (AllowSharedMemory && m_pSrcDevice && m_pDstDevice)
    {
#if VULKAN_SUPPORTED
        if (m_Mode == TransferMode::CpuCopy)
        {
            if (CreateSharedHostMemoryTextures(m_pSrcDevice, m_pDstDevice, TransferDesc, m_Buffers))
                m_Mode = TransferMode::SharedHostMemory;
        }
#endif
#if D3D12_SUPPORTED
        if (m_Mode == TransferMode::CpuCopy)
        {
            if (CreateSharedCrossAdapterTextures(m_pSrcDevice, m_pDstDevice, TransferDesc, m_Buffers))
                m_Mode = TransferMode::SharedCrossAdapterTexture;
        }
#endif
    }

    if (m_Mode == TransferMode::CpuCopy)
    {
        // Source-side staging texture: GPU renders into a render target, then copies
        // to this CPU-readable staging texture via CopySourceFrame.
        TextureDesc StagingReadbackDesc    = TransferDesc;
        StagingReadbackDesc.Name           = "CrossDevice Staging Readback (Src)";
        StagingReadbackDesc.BindFlags      = BIND_NONE;
        StagingReadbackDesc.MiscFlags      = MISC_TEXTURE_FLAG_NONE;
        StagingReadbackDesc.Usage          = USAGE_STAGING;
        StagingReadbackDesc.CPUAccessFlags = CPU_ACCESS_READ;

        // Destination-side staging texture: CPU-writable staging buffer that receives
        // pixel data from the source staging readback, then gets copied to the final texture.
        TextureDesc StagingUploadDesc    = StagingReadbackDesc;
        StagingUploadDesc.Name           = "CrossDevice Staging Upload (Dst)";
        StagingUploadDesc.CPUAccessFlags = CPU_ACCESS_WRITE;

        for (FrameBufferTransfer& Buffer : m_Buffers)
        {
            // A failed attempt to share memory may have left some textures
            Buffer.pSecondaryStagingReadback.Release();
            Buffer.pPrimaryStagingUpload.Release();

            if (m_pSrcDevice)
                m_pSrcDevice->CreateTexture(StagingReadbackDesc, nullptr, &Buffer.pSecondaryStagingReadback);
            if (m_pDstDevice)
                m_pDstDevice->CreateTexture(StagingUploadDesc, nullptr, &Buffer.pPrimaryStagingUpload);
        }
    }

    if (m_pSrcDevice && m_pDstDevice)
        LOG_INFO_MESSAGE("CrossDeviceTransferManager: frames are transferred through ", GetTransferModeString(m_Mode));

    m_Initialized = true;
}

void CrossDeviceTransferManager::CopySourceFrame(IDeviceContext* pSrcContext,
                                                 ITexture*       pSrcTexture,
                                                 Uint32          FrameId)
{
    VERIFY_EXPR(pSrcContext != nullptr && pSrcTexture != nullptr);
    if (!pSrcContext || !pSrcTexture)
        return;

    std::lock_guard<std::mutex> Lock{m_Mtx};
    if (!m_Initialized)
        return;
    Uint32 slot = FrameId % m_Latency;

    FrameBufferTransfer& Transfer = m_Buffers[slot];
    if (!Transfer.pSecondaryStagingReadback)
        return;

    // Shared modes: the source GPU writes the memory that the destination GPU may still be reading
    // (the copy of this slot m_Latency transfers ago). The devices can not wait for each other's
    // fences, so the CPU waits. TransferReadyFrame() flushes that copy, so the wait completes.
    if (IsSharedMode() && Transfer.pPrimaryFence && Transfer.pPrimaryFence->GetCompletedValue() < Transfer.PrimaryFenceValue)
        Transfer.pPrimaryFence->Wait(Transfer.PrimaryFenceValue);

    // Copy the rendered frame from the source render target to the transfer memory.
    CopyTextureAttribs copyAttribs(pSrcTexture, RESOURCE_STATE_TRANSITION_MODE_TRANSITION,
                                   Transfer.pSecondaryStagingReadback, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    pSrcContext->CopyTexture(copyAttribs);

    if (m_Mode == TransferMode::SharedCrossAdapterTexture)
        TransitionToCommon(pSrcContext, Transfer.pSecondaryStagingReadback);

    // Signal the fence so the destination side knows when the copy is complete.
    Transfer.FenceValue++;
    pSrcContext->EnqueueSignal(Transfer.pSecondaryFence, Transfer.FenceValue);
    // A signal is only executed once the context is flushed. Without this, TransferReadyFrame()
    // would wait for a value that is never signaled if the application does not flush in between.
    pSrcContext->Flush();
}

bool CrossDeviceTransferManager::TransferReadyFrame(IDeviceContext* pSrcContext,
                                                    IDeviceContext* pDstContext,
                                                    ITexture*       pDstTexture,
                                                    Uint32          FrameId)
{
    std::lock_guard<std::mutex> Lock{m_Mtx};
    if (!m_Initialized)
        return false;

    // During the initial latency window, we are still filling the pipeline. This is not an error,
    // so it is checked before the arguments are validated.
    if (FrameId < m_Latency - 1)
        return false;

    VERIFY_EXPR(pSrcContext != nullptr && pDstContext != nullptr && pDstTexture != nullptr);
    if (!pSrcContext || !pDstContext || !pDstTexture)
        return false;
    Uint32 readSlot = (FrameId - (m_Latency - 1)) % m_Latency;

    FrameBufferTransfer& Transfer = m_Buffers[readSlot];

    // Wait for the source GPU to finish copying into the transfer memory.
    if (Transfer.pSecondaryFence)
    {
        Uint64 completed = Transfer.pSecondaryFence->GetCompletedValue();
        if (completed < Transfer.FenceValue)
        {
            Transfer.pSecondaryFence->Wait(Transfer.FenceValue);
        }
    }

    // Ensure both transfer textures are available.
    if (!Transfer.pSecondaryStagingReadback || !Transfer.pPrimaryStagingUpload)
        return false;

    if (IsSharedMode())
    {
        // The destination GPU copies the frame out of the memory that the source GPU wrote.
        // The CPU does not touch the frame data.
        if (m_Mode == TransferMode::SharedCrossAdapterTexture)
        {
            // The source adapter left the texture in the COMMON state
            Transfer.pPrimaryStagingUpload->SetState(RESOURCE_STATE_COMMON);
        }

        CopyTextureAttribs dstCopyAttribs(Transfer.pPrimaryStagingUpload, RESOURCE_STATE_TRANSITION_MODE_TRANSITION,
                                          pDstTexture, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        pDstContext->CopyTexture(dstCopyAttribs);

        if (m_Mode == TransferMode::SharedCrossAdapterTexture)
            TransitionToCommon(pDstContext, Transfer.pPrimaryStagingUpload);

        if (Transfer.pPrimaryFence)
            pDstContext->EnqueueSignal(Transfer.pPrimaryFence, ++Transfer.PrimaryFenceValue);
        // CopySourceFrame() waits for this signal before the source GPU overwrites the slot
        pDstContext->Flush();
        return true;
    }

    // Map the source staging readback texture to get CPU access to pixel data.
    // This must use the source device's context since the texture belongs to the source device.
    MappedTextureSubresource SrcMapped{};
    pSrcContext->MapTextureSubresource(Transfer.pSecondaryStagingReadback, 0, 0, MAP_READ, MAP_FLAG_NONE, nullptr, SrcMapped);
    if (SrcMapped.pData == nullptr)
    {
        LOG_ERROR_MESSAGE("CrossDeviceTransferManager: Failed to map source staging readback texture.");
        return false;
    }

    // The destination GPU may still be copying out of this slot's upload texture (submitted
    // m_Latency transfers ago). Wait for that copy before the CPU overwrites the texture; the
    // destination context is flushed only if the copy has not been submitted yet.
    if (Transfer.pPrimaryFence && Transfer.pPrimaryFence->GetCompletedValue() < Transfer.PrimaryFenceValue)
    {
        pDstContext->Flush();
        Transfer.pPrimaryFence->Wait(Transfer.PrimaryFenceValue);
    }

    // Map the destination staging upload texture for writing.
    // This uses the destination device's context.
    MappedTextureSubresource DstMapped{};
    pDstContext->MapTextureSubresource(Transfer.pPrimaryStagingUpload, 0, 0, MAP_WRITE, MAP_FLAG_NONE, nullptr, DstMapped);
    if (DstMapped.pData == nullptr)
    {
        pSrcContext->UnmapTextureSubresource(Transfer.pSecondaryStagingReadback, 0, 0);
        LOG_ERROR_MESSAGE("CrossDeviceTransferManager: Failed to map destination staging upload texture.");
        return false;
    }

    // Copy pixel data row by row from source staging to destination staging.
    const TextureFormatAttribs& FmtAttribs = GetTextureFormatAttribs(m_TexDesc.Format);
    const Uint32 RowSize = (m_TexDesc.Width / FmtAttribs.BlockWidth) * FmtAttribs.GetElementSize();
    const Uint32 NumRows = m_TexDesc.Height / FmtAttribs.BlockHeight;

    const Uint8* pSrcRow = static_cast<const Uint8*>(SrcMapped.pData);
    Uint8*       pDstRow = static_cast<Uint8*>(DstMapped.pData);

    for (Uint32 row = 0; row < NumRows; ++row)
    {
        std::memcpy(pDstRow, pSrcRow, RowSize);
        pSrcRow += SrcMapped.Stride;
        pDstRow += DstMapped.Stride;
    }

    pSrcContext->UnmapTextureSubresource(Transfer.pSecondaryStagingReadback, 0, 0);
    pDstContext->UnmapTextureSubresource(Transfer.pPrimaryStagingUpload, 0, 0);

    // Copy from the destination staging upload texture to the final destination texture.
    CopyTextureAttribs dstCopyAttribs(Transfer.pPrimaryStagingUpload, RESOURCE_STATE_TRANSITION_MODE_TRANSITION,
                                      pDstTexture, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    pDstContext->CopyTexture(dstCopyAttribs);
    if (Transfer.pPrimaryFence)
        pDstContext->EnqueueSignal(Transfer.pPrimaryFence, ++Transfer.PrimaryFenceValue);

    return true;
}

void CrossDeviceTransferManager::WaitForFrame(Uint32 FrameId)
{
    std::lock_guard<std::mutex> Lock{m_Mtx};
    if (!m_Initialized)
        return;
    Uint32 slot = FrameId % m_Latency;
    if (m_Buffers[slot].pSecondaryFence && m_Buffers[slot].FenceValue > 0)
    {
        m_Buffers[slot].pSecondaryFence->Wait(m_Buffers[slot].FenceValue);
    }
}

void CrossDeviceTransferManager::Reset()
{
    // Shared host memory is owned by the textures, which release it after the devices are done with it
    m_Buffers.clear();
    m_pSrcDevice.Release();
    m_pDstDevice.Release();
    m_Initialized = false;
    m_Mode        = TransferMode::None;
}

} // namespace Diligent
