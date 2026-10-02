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
#include <cstring>
#include "../../../Platforms/Basic/interface/DebugUtilities.hpp"
#include "../../GraphicsAccessories/interface/GraphicsAccessories.hpp"

namespace Diligent
{

void CrossDeviceTransferManager::Init(IRenderDevice*     pSrcDevice,
                                      IRenderDevice*     pDstDevice,
                                      const TextureDesc& Desc,
                                      Uint32             FrameLatency)
{
    std::lock_guard<std::mutex> Lock{m_Mtx};

    Reset();

    m_pSrcDevice = pSrcDevice;
    m_pDstDevice = pDstDevice;
    m_TexDesc    = Desc;
    m_Latency    = (std::max)(1u, FrameLatency);
    m_Buffers.resize(m_Latency);

    // Source-side staging texture: GPU renders into a render target, then copies
    // to this CPU-readable staging texture via CopySourceFrame.
    TextureDesc StagingReadbackDesc = Desc;
    StagingReadbackDesc.Name           = "CrossDevice Staging Readback (Src)";
    StagingReadbackDesc.BindFlags      = BIND_NONE;
    StagingReadbackDesc.Usage          = USAGE_STAGING;
    StagingReadbackDesc.CPUAccessFlags = CPU_ACCESS_READ;
    StagingReadbackDesc.MipLevels      = 1;
    StagingReadbackDesc.ArraySize      = 1;

    // Destination-side staging texture: CPU-writable staging buffer that receives
    // pixel data from the source staging readback, then gets copied to the final texture.
    TextureDesc StagingUploadDesc = Desc;
    StagingUploadDesc.Name           = "CrossDevice Staging Upload (Dst)";
    StagingUploadDesc.BindFlags      = BIND_NONE;
    StagingUploadDesc.Usage          = USAGE_STAGING;
    StagingUploadDesc.CPUAccessFlags = CPU_ACCESS_WRITE;
    StagingUploadDesc.MipLevels      = 1;
    StagingUploadDesc.ArraySize      = 1;

    FenceDesc fenceDesc;
    fenceDesc.Name = "CrossDevice Transfer Fence";

    for (Uint32 i = 0; i < m_Latency; ++i)
    {
        if (m_pSrcDevice)
        {
            m_pSrcDevice->CreateTexture(StagingReadbackDesc, nullptr, &m_Buffers[i].pSecondaryStagingReadback);
            m_pSrcDevice->CreateFence(fenceDesc, &m_Buffers[i].pSecondaryFence);
        }

        if (m_pDstDevice)
        {
            m_pDstDevice->CreateTexture(StagingUploadDesc, nullptr, &m_Buffers[i].pPrimaryStagingUpload);
            m_pDstDevice->CreateFence(fenceDesc, &m_Buffers[i].pPrimaryFence);
        }
    }

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

    // Copy the rendered frame from the source render target to the CPU-readable staging texture.
    CopyTextureAttribs copyAttribs(pSrcTexture, RESOURCE_STATE_TRANSITION_MODE_TRANSITION,
                                   m_Buffers[slot].pSecondaryStagingReadback, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    pSrcContext->CopyTexture(copyAttribs);

    // Signal the fence so the destination side knows when the copy is complete.
    m_Buffers[slot].FenceValue++;
    pSrcContext->EnqueueSignal(m_Buffers[slot].pSecondaryFence, m_Buffers[slot].FenceValue);
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

    // Wait for the source GPU to finish copying into the staging readback texture.
    if (Transfer.pSecondaryFence)
    {
        Uint64 completed = Transfer.pSecondaryFence->GetCompletedValue();
        if (completed < Transfer.FenceValue)
        {
            Transfer.pSecondaryFence->Wait(Transfer.FenceValue);
        }
    }

    // Ensure both staging textures are available.
    if (!Transfer.pSecondaryStagingReadback || !Transfer.pPrimaryStagingUpload)
        return false;

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
    m_Buffers.clear();
    m_pSrcDevice.Release();
    m_pDstDevice.Release();
    m_Initialized = false;
}

} // namespace Diligent
