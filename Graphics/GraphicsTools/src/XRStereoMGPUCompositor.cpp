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

#include "XRStereoMGPUCompositor.hpp"

#include "DebugUtilities.hpp"

namespace Diligent
{

void XRStereoMGPUCompositor::Initialize(const CreateInfo& CI, const TextureDesc& EyeDesc)
{
    DEV_CHECK_ERR(CI.pPrimaryDevice != nullptr, "Primary device must not be null");

    m_pPrimaryDevice = CI.pPrimaryDevice;
    m_Linked         = CI.Linked;
    m_EyeDesc        = EyeDesc;

    if (m_Linked)
    {
        // In linked mode a single device spans all nodes; the secondary device is the same object.
        m_pSecondaryDevice = CI.pSecondaryDevice != nullptr ? CI.pSecondaryDevice : CI.pPrimaryDevice;
    }
    else
    {
        DEV_CHECK_ERR(CI.pSecondaryDevice != nullptr, "Unlinked mode requires a distinct secondary device");
        m_pSecondaryDevice = CI.pSecondaryDevice;

        // Set up the pipelined inter-device transfer (secondary GPU -> primary GPU).
        m_Transfer.Init(CI.pSecondaryDevice, CI.pPrimaryDevice, EyeDesc, CI.FrameLatency);

        // Landing texture on the primary device that receives the transferred eye before it is copied
        // into the swapchain slice. It is a plain, non-array render target/copy target.
        TextureDesc LandingDesc = EyeDesc;
        LandingDesc.Name        = "XR stereo mGPU primary eye landing texture";
        LandingDesc.Type        = RESOURCE_DIM_TEX_2D;
        LandingDesc.ArraySize   = 1;
        if (LandingDesc.BindFlags == BIND_NONE)
            LandingDesc.BindFlags = BIND_SHADER_RESOURCE;
        m_pPrimaryDevice->CreateTexture(LandingDesc, nullptr, &m_pPrimaryEyeLanding);
        DEV_CHECK_ERR(m_pPrimaryEyeLanding != nullptr, "Failed to create primary eye landing texture");
    }

    m_Initialized = true;
}

bool XRStereoMGPUCompositor::Composite(const CompositeAttribs& Attribs)
{
    DEV_CHECK_ERR(m_Initialized, "Compositor is not initialized");
    DEV_CHECK_ERR(Attribs.pSecondaryEye != nullptr, "Secondary eye texture must not be null");
    DEV_CHECK_ERR(Attribs.pXRSwapchainTexture != nullptr, "Destination swapchain texture must not be null");
    DEV_CHECK_ERR(Attribs.pPrimaryContext != nullptr, "Primary context must not be null");

    if (m_Linked)
    {
        // Linked multi-GPU: the secondary eye texture is peer-visible (created with a VisibleNodeMask
        // that includes node 0), so the primary context can read it directly. Copy it into the target
        // slice of the OpenXR swapchain texture over the peer memory link (NVLink/PCIe).
        CopyTextureAttribs CopyAttribs{
            Attribs.pSecondaryEye, RESOURCE_STATE_TRANSITION_MODE_TRANSITION,
            Attribs.pXRSwapchainTexture, RESOURCE_STATE_TRANSITION_MODE_TRANSITION};
        CopyAttribs.SrcMipLevel = 0;
        CopyAttribs.SrcSlice    = 0;
        CopyAttribs.DstMipLevel = Attribs.DstMipLevel;
        CopyAttribs.DstSlice    = Attribs.DstArraySlice;
        Attribs.pPrimaryContext->CopyTexture(CopyAttribs);
        return true;
    }
    else
    {
        DEV_CHECK_ERR(Attribs.pSecondaryContext != nullptr, "Unlinked mode requires a secondary context");

        // Unlinked multi-GPU: pipeline the eye from the secondary device to the primary device.
        // 1) Record a readback of the current secondary eye on the secondary device.
        m_Transfer.CopySourceFrame(Attribs.pSecondaryContext, Attribs.pSecondaryEye, Attribs.FrameId);

        // 2) Upload the oldest ready frame into the primary-device landing texture. This returns false
        //    while the pipeline is still filling (first FrameLatency frames), in which case there is
        //    nothing to composite yet.
        if (!m_Transfer.TransferReadyFrame(Attribs.pSecondaryContext, Attribs.pPrimaryContext,
                                           m_pPrimaryEyeLanding, Attribs.FrameId))
        {
            return false;
        }

        // 3) Copy the landed eye into the target slice of the OpenXR swapchain texture on the primary device.
        CopyTextureAttribs CopyAttribs{
            m_pPrimaryEyeLanding, RESOURCE_STATE_TRANSITION_MODE_TRANSITION,
            Attribs.pXRSwapchainTexture, RESOURCE_STATE_TRANSITION_MODE_TRANSITION};
        CopyAttribs.DstMipLevel = Attribs.DstMipLevel;
        CopyAttribs.DstSlice    = Attribs.DstArraySlice;
        Attribs.pPrimaryContext->CopyTexture(CopyAttribs);
        return true;
    }
}

void XRStereoMGPUCompositor::Reset()
{
    m_Transfer.Reset();
    m_pPrimaryEyeLanding.Release();
    m_pSecondaryDevice.Release();
    m_pPrimaryDevice.Release();
    m_Initialized = false;
    m_Linked      = false;
}

} // namespace Diligent
