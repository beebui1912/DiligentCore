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

/// \file
/// Helper for stereo multi-GPU (mGPU) compositing into an OpenXR swapchain texture.
///
/// In VR, a common and effective mGPU pattern renders one eye on the primary GPU node and the other
/// eye on a secondary node/device. The OpenXR runtime always allocates its swapchain textures on the
/// primary presenting adapter (node 0), so the secondary eye must be brought back to node 0 before
/// `xrEndFrame`. This helper performs that final composition step and works with both:
///
///   * Linked multi-GPU (LDA / Vulkan device groups): a single IRenderDevice manages several nodes,
///     and the secondary eye texture is peer-visible (created with a VisibleNodeMask that includes
///     node 0). The composition is a direct cross-node CopyTexture on the primary context.
///
///   * Unlinked multi-GPU: two independent IRenderDevice objects. The composition is pipelined
///     through Diligent::CrossDeviceTransferManager (readback on the secondary device, upload on the
///     primary device), then copied into the target swapchain slice.
///
/// The helper is purely additive: existing single-GPU OpenXR code that renders both eyes on node 0
/// does not use it and is unaffected.

#include "../../GraphicsEngine/interface/RenderDevice.h"
#include "../../GraphicsEngine/interface/DeviceContext.h"
#include "../../GraphicsEngine/interface/Texture.h"
#include "../../../Common/interface/RefCntAutoPtr.hpp"

#include "CrossDeviceTransferManager.hpp"

namespace Diligent
{

/// Composites a secondary-node/secondary-device eye texture into an OpenXR swapchain texture that
/// lives on the primary presenting device.
class XRStereoMGPUCompositor
{
public:
    struct CreateInfo
    {
        /// Primary presenting device (node 0). The OpenXR swapchain textures belong to this device.
        IRenderDevice* pPrimaryDevice = nullptr;

        /// Secondary rendering device. In linked mode this must be the same object as pPrimaryDevice
        /// (a single device that spans multiple nodes). In unlinked mode this is a distinct device.
        IRenderDevice* pSecondaryDevice = nullptr;

        /// True for linked multi-GPU (device groups / LDA), false for unlinked (two devices).
        bool Linked = false;

        /// Pipelined frame latency for the unlinked cross-device transfer (ignored in linked mode).
        Uint32 FrameLatency = 2;
    };

    XRStereoMGPUCompositor() noexcept = default;
    ~XRStereoMGPUCompositor()         = default;

    /// Initializes the compositor.
    ///
    /// \param [in] CI      - Creation parameters (devices and mode).
    /// \param [in] EyeDesc - Description of a single eye texture (color format and dimensions). Used to
    ///                       size the intermediate staging resources in unlinked mode.
    void Initialize(const CreateInfo& CI, const TextureDesc& EyeDesc);

    struct CompositeAttribs
    {
        /// Source texture holding the eye rendered on the secondary node/device.
        ITexture* pSecondaryEye = nullptr;

        /// Destination OpenXR swapchain texture on the primary device (from GetOpenXRSwapchainImage).
        ITexture* pXRSwapchainTexture = nullptr;

        /// Array slice of the destination swapchain texture that receives the eye (e.g. 1 for the
        /// right eye of a 2-slice texture-array swapchain).
        Uint32 DstArraySlice = 1;

        /// Destination mip level (usually 0).
        Uint32 DstMipLevel = 0;

        /// Primary device context (records the copy/upload into the swapchain texture).
        IDeviceContext* pPrimaryContext = nullptr;

        /// Secondary device context. In linked mode this may be the same as pPrimaryContext.
        IDeviceContext* pSecondaryContext = nullptr;

        /// Frame index, used to pipeline the unlinked transfer (ignored in linked mode).
        Uint32 FrameId = 0;
    };

    /// Composites the secondary eye into the target swapchain slice.
    ///
    /// \return In linked mode, always true after issuing the copy. In unlinked mode, true when a
    ///         pipelined frame was ready and uploaded, false while the pipeline is still filling.
    bool Composite(const CompositeAttribs& Attribs);

    /// Releases all resources.
    void Reset();

    /// Returns true if the compositor has been initialized.
    bool IsInitialized() const { return m_Initialized; }

    /// Returns true if the compositor operates in linked multi-GPU mode.
    bool IsLinked() const { return m_Linked; }

private:
    RefCntAutoPtr<IRenderDevice> m_pPrimaryDevice;
    RefCntAutoPtr<IRenderDevice> m_pSecondaryDevice;
    TextureDesc                  m_EyeDesc;
    bool                         m_Linked      = false;
    bool                         m_Initialized = false;

    // Unlinked mode only: pipelined inter-device transfer and the primary-device landing texture that
    // receives the transferred eye before it is copied into the swapchain slice.
    CrossDeviceTransferManager m_Transfer;
    RefCntAutoPtr<ITexture>    m_pPrimaryEyeLanding;
};

} // namespace Diligent
