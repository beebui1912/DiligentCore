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
///
/// When both devices can share memory, the GPUs copy the frame through that memory (see TransferMode).
/// Otherwise the CPU copies every frame between staging textures.
///
/// When the devices share memory, they are synchronized in one of two ways:
/// * On the GPU, if they can also share fences (CreateCrossDeviceFence()): each device's queue waits for
///   the other device's signal, and the CPU never blocks (UsesGpuSynchronization() returns true).
/// * Otherwise the CPU waits for the fence of one device before it submits the work of the other.
/// The choice is made at run time, so the same code runs on every platform and backend.
class CrossDeviceTransferManager
{
public:
    /// How the frame data moves from the source device to the destination device
    enum class TransferMode : Uint8
    {
        /// The manager is not initialized.
        None = 0,

        /// The CPU copies every frame from a readback texture of the source device
        /// into an upload texture of the destination device. Works with any two devices.
        CpuCopy,

        /// Vulkan: both devices import one host allocation (VK_EXT_external_memory_host).
        /// The source GPU copies the frame into it and the destination GPU copies it out.
        SharedHostMemory,

        /// Direct3D12: both devices open one cross-adapter texture (D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER,
        /// row-major layout, in system memory). The source GPU copies the frame into it and the destination
        /// GPU copies it out.
        SharedCrossAdapterTexture,
    };

    /// Returns a string with the name of the transfer mode.
    static const char* GetTransferModeString(TransferMode Mode);

    struct FrameBufferTransfer
    {
        /// CpuCopy: the readback texture of the source device.
        /// Shared modes: the source device's texture in the shared memory.
        RefCntAutoPtr<ITexture> pSecondaryStagingReadback;

        /// CpuCopy: the upload texture of the destination device.
        /// Shared modes: the destination device's texture in the same shared memory.
        RefCntAutoPtr<ITexture> pPrimaryStagingUpload;

        /// Signaled on the source device after the copy into pSecondaryStagingReadback.
        RefCntAutoPtr<IFence> pSecondaryFence;
        Uint64                FenceValue = 0;

        /// Signaled on the destination device after the copy out of pPrimaryStagingUpload, so that
        /// the slot is not overwritten (by the CPU, or by the source GPU in the shared modes)
        /// while that copy is still pending.
        RefCntAutoPtr<IFence> pPrimaryFence;
        Uint64                PrimaryFenceValue = 0;

        /// GPU synchronization only: the destination device's fence that shares pSecondaryFence.
        /// The destination queue waits for it before the copy out of pPrimaryStagingUpload.
        RefCntAutoPtr<IFence> pSecondaryFenceOnDst;

        /// GPU synchronization only: the source device's fence that shares pPrimaryFence.
        /// The source queue waits for it before the copy into pSecondaryStagingReadback.
        RefCntAutoPtr<IFence> pPrimaryFenceOnSrc;
    };

    CrossDeviceTransferManager() noexcept = default;
    ~CrossDeviceTransferManager()         = default;

    /// Initializes the transfer manager with source and destination devices.
    ///
    /// \param [in] pSrcDevice        - Pointer to the source render device (e.g. secondary rendering GPU).
    /// \param [in] pDstDevice        - Pointer to the destination render device (e.g. primary presenting GPU).
    /// \param [in] Desc              - Texture description matching the transferred frame.
    /// \param [in] FrameLatency      - Pipelined buffer latency (typically 2 for double buffering).
    /// \param [in] AllowSharedMemory - Whether the devices may share memory (TransferMode::SharedHostMemory or
    ///                                 TransferMode::SharedCrossAdapterTexture). If false, or if the devices can not
    ///                                 share memory, the CPU copies the frames (TransferMode::CpuCopy).
    /// \param [in] AllowGpuSynchronization - Whether the devices may share fences to synchronize on the GPU when they
    ///                                 share memory (see UsesGpuSynchronization()). If false, or if the devices can not
    ///                                 share fences, the CPU waits for the fences.
    void Init(IRenderDevice*     pSrcDevice,
              IRenderDevice*     pDstDevice,
              const TextureDesc& Desc,
              Uint32             FrameLatency            = 2,
              bool               AllowSharedMemory       = true,
              bool               AllowGpuSynchronization = true);

    /// Records a copy of the source render target to the transfer memory on the source GPU.
    ///
    /// In the shared modes, the copy must wait until the destination GPU has finished reading the slot
    /// FrameLatency transfers ago (usually completed already). With GPU synchronization the source queue
    /// waits; the wait applies to the whole submission, including the commands recorded before this call.
    /// Otherwise the CPU waits.
    ///
    /// \param [in] pSrcContext - Pointer to the source device context.
    /// \param [in] pSrcTexture - Pointer to the source texture/render target to copy from.
    /// \param [in] FrameId     - Frame index.
    void CopySourceFrame(IDeviceContext* pSrcContext,
                         ITexture*       pSrcTexture,
                         Uint32          FrameId);

    /// Uploads the transferred frame data into the destination texture on the destination GPU.
    ///
    /// The upload must wait until the source GPU has finished the copy of the frame: with GPU synchronization
    /// the destination queue waits, otherwise the CPU waits. In the shared modes, the destination GPU then copies
    /// the frame out of the shared memory and the destination context is flushed; the CPU does not access the
    /// frame data.
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

    /// Returns how the frame data moves between the devices.
    TransferMode GetTransferMode() const { return m_Mode; }

    /// Returns true if the devices wait for each other on the GPU through shared fences,
    /// and false if the CPU waits for the fences.
    bool UsesGpuSynchronization() const { return m_GpuSync; }

private:
    bool IsSharedMode() const { return m_Mode == TransferMode::SharedHostMemory || m_Mode == TransferMode::SharedCrossAdapterTexture; }

    RefCntAutoPtr<IRenderDevice>     m_pSrcDevice;
    RefCntAutoPtr<IRenderDevice>     m_pDstDevice;
    TextureDesc                      m_TexDesc;
    Uint32                           m_Latency     = 2;
    bool                             m_Initialized = false;
    TransferMode                     m_Mode        = TransferMode::None;
    bool                             m_GpuSync     = false;
    std::vector<FrameBufferTransfer> m_Buffers;
    std::mutex                       m_Mtx;
};

} // namespace Diligent
