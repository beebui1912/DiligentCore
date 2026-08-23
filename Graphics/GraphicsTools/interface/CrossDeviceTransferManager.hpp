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

#include <vector>
#include <mutex>

#include "../../GraphicsEngine/interface/RenderDevice.h"
#include "../../GraphicsEngine/interface/DeviceContext.h"
#include "../../GraphicsEngine/interface/Texture.h"
#include "../../GraphicsEngine/interface/Fence.h"
#include "../../../Common/interface/RefCntAutoPtr.hpp"

namespace Diligent
{

/// Helper class for pipelined inter-GPU resource transfer in Unlinked Multi-GPU configurations.
class CrossDeviceTransferManager
{
public:
    struct FrameBufferTransfer
    {
        RefCntAutoPtr<ITexture> pSecondaryStagingReadback;
        RefCntAutoPtr<ITexture> pPrimaryStagingUpload;
        RefCntAutoPtr<IFence>   pSecondaryFence;
        Uint64                  FenceValue = 0;

        /// Signaled on the destination device after the copy out of pPrimaryStagingUpload, so that
        /// the slot is not overwritten by the CPU while that copy is still pending.
        RefCntAutoPtr<IFence> pPrimaryFence;
        Uint64                PrimaryFenceValue = 0;
    };

    CrossDeviceTransferManager() noexcept = default;
    ~CrossDeviceTransferManager()         = default;

    /// Initializes the transfer manager with source and destination devices.
    ///
    /// \param [in] pSrcDevice    - Pointer to the source render device (e.g. secondary rendering GPU).
    /// \param [in] pDstDevice    - Pointer to the destination render device (e.g. primary presenting GPU).
    /// \param [in] Desc          - Texture description matching the transferred frame.
    /// \param [in] FrameLatency  - Pipelined buffer latency (typically 2 for double buffering).
    void Init(IRenderDevice*     pSrcDevice,
              IRenderDevice*     pDstDevice,
              const TextureDesc& Desc,
              Uint32             FrameLatency = 2);

    /// Records a copy of the source render target to the host-visible staging buffer on the source GPU.
    ///
    /// \param [in] pSrcContext - Pointer to the source device context.
    /// \param [in] pSrcTexture - Pointer to the source texture/render target to copy from.
    /// \param [in] FrameId     - Frame index.
    void CopySourceFrame(IDeviceContext* pSrcContext,
                         ITexture*       pSrcTexture,
                         Uint32          FrameId);

    /// Uploads the transferred frame data into the destination texture on the destination GPU.
    ///
    /// \param [in] pSrcContext    - Pointer to the source device context (needed to map the readback staging texture).
    /// \param [in] pDstContext    - Pointer to the destination device context.
    /// \param [in] pDstTexture    - Pointer to the destination texture to receive data.
    /// \param [in] FrameId        - Frame index.
    /// \return                    - True if frame data was ready and transferred, false otherwise.
    bool TransferReadyFrame(IDeviceContext* pSrcContext,
                            IDeviceContext* pDstContext,
                            ITexture*       pDstTexture,
                            Uint32          FrameId);

    /// Waits for completion of a specific frame's copy on the source device.
    void WaitForFrame(Uint32 FrameId);

    /// Resets all allocated resources.
    void Reset();

    /// Returns the configured frame latency.
    Uint32 GetLatency() const { return m_Latency; }

    /// Returns true if initialized.
    bool IsInitialized() const { return m_Initialized; }

private:
    RefCntAutoPtr<IRenderDevice>     m_pSrcDevice;
    RefCntAutoPtr<IRenderDevice>     m_pDstDevice;
    TextureDesc                      m_TexDesc;
    Uint32                           m_Latency = 2;
    bool                             m_Initialized = false;
    std::vector<FrameBufferTransfer> m_Buffers;
    std::mutex                       m_Mtx;
};

} // namespace Diligent
