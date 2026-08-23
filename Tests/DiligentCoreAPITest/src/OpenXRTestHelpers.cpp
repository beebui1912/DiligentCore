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

#include "OpenXRTestHelpers.hpp"

#include <chrono>
#include <cstdio>
#include <iostream>
#include <thread>
#include <vector>

#include "OpenXRUtilities.h"
#include "GraphicsUtilities.h"
#include "GraphicsAccessories.hpp"
#include "RefCntAutoPtr.hpp"

#include "gtest/gtest.h"

namespace Diligent
{

namespace Testing
{

OpenXRTestInstance::~OpenXRTestInstance()
{
    if (m_Instance != XR_NULL_HANDLE)
        xrDestroyInstance(m_Instance);
}

std::string OpenXRTestInstance::Create(const char* GraphicsExtension)
{
    const char* Extensions[] = {GraphicsExtension};

    XrInstanceCreateInfo InstanceCI{XR_TYPE_INSTANCE_CREATE_INFO};
    std::snprintf(InstanceCI.applicationInfo.applicationName, XR_MAX_APPLICATION_NAME_SIZE, "%s", "DiligentCoreAPITest");
    InstanceCI.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
    InstanceCI.enabledExtensionCount      = 1;
    InstanceCI.enabledExtensionNames      = Extensions;

    const XrResult res = xrCreateInstance(&InstanceCI, &m_Instance);
    if (XR_FAILED(res))
    {
        m_Instance = XR_NULL_HANDLE;
        return std::string{"No OpenXR runtime with "} + GraphicsExtension + " (xrCreateInstance: " + std::to_string(res) + ")";
    }

    XrInstanceProperties InstanceProps{XR_TYPE_INSTANCE_PROPERTIES};
    xrGetInstanceProperties(m_Instance, &InstanceProps);
    std::cout << "    OpenXR runtime: " << InstanceProps.runtimeName << std::endl;

    XrSystemGetInfo SystemGI{XR_TYPE_SYSTEM_GET_INFO};
    SystemGI.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    if (XR_FAILED(xrGetSystem(m_Instance, &SystemGI, &m_SystemId)))
        return "The OpenXR runtime has no head-mounted display";

    return {};
}

OpenXRAttribs OpenXRTestInstance::GetAttribs() const
{
    OpenXRAttribs Attribs;
    Attribs.GetInstanceProcAddr = reinterpret_cast<void*>(xrGetInstanceProcAddr);
    Attribs.Instance            = reinterpret_cast<Uint64>(m_Instance);
    Attribs.SystemId            = m_SystemId;
    return Attribs;
}

PFN_xrVoidFunction OpenXRTestInstance::GetProcAddr(const char* Name) const
{
    PFN_xrVoidFunction Func = nullptr;
    if (XR_FAILED(xrGetInstanceProcAddr(m_Instance, Name, &Func)))
        return nullptr;
    return Func;
}

Uint8 FindAdapterQueue(const GraphicsAdapterInfo& Adapter, COMMAND_QUEUE_TYPE Type)
{
    for (Uint32 q = 0; q < Adapter.NumQueues; ++q)
    {
        if ((Adapter.Queues[q].QueueType & COMMAND_QUEUE_TYPE_PRIMARY_MASK) == Type)
            return static_cast<Uint8>(q);
    }
    return DEFAULT_QUEUE_ID;
}

namespace
{

// Session objects use the application's device, so they are destroyed while the device is alive
struct XrSessionObjects
{
    IRenderDevice* pDevice   = nullptr;
    XrSession      Session   = XR_NULL_HANDLE;
    XrSpace        Space     = XR_NULL_HANDLE;
    XrSwapchain    Swapchain = XR_NULL_HANDLE;

    ~XrSessionObjects()
    {
        if (pDevice != nullptr)
            pDevice->IdleGPU();
        if (Swapchain != XR_NULL_HANDLE)
            xrDestroySwapchain(Swapchain);
        if (Space != XR_NULL_HANDLE)
            xrDestroySpace(Space);
        if (Session != XR_NULL_HANDLE)
            xrDestroySession(Session);
    }
};

// Waits for the session state, processing other events. Returns false on timeout.
bool WaitForSessionState(XrInstance Instance, XrSessionState State, std::chrono::milliseconds Timeout)
{
    const auto StartTime = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - StartTime < Timeout)
    {
        XrEventDataBuffer Event{XR_TYPE_EVENT_DATA_BUFFER};
        while (xrPollEvent(Instance, &Event) == XR_SUCCESS)
        {
            if (Event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED &&
                reinterpret_cast<const XrEventDataSessionStateChanged*>(&Event)->state == State)
                return true;
            Event = {XR_TYPE_EVENT_DATA_BUFFER};
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    return false;
}

} // namespace

void RunOpenXRFrames(const OpenXRTestInstance& Xr,
                     IRenderDevice*            pDevice,
                     IDeviceContext*           pContext,
                     Uint32                    FramesToSubmit)
{
    ASSERT_NE(pDevice, nullptr);
    ASSERT_NE(pContext, nullptr);

    const RENDER_DEVICE_TYPE DeviceType = pDevice->GetDeviceInfo().Type;
    const XrInstance         Instance   = Xr.GetInstance();
    const XrSystemId         SystemId   = Xr.GetSystemId();

    XrSessionObjects XrS;
    XrS.pDevice = pDevice;

    // Session
    {
        RefCntAutoPtr<IDataBlob> pBinding;
        GetOpenXRGraphicsBinding(pDevice, pContext, &pBinding);
        ASSERT_NE(pBinding, nullptr);

        XrSessionCreateInfo SessionCI{XR_TYPE_SESSION_CREATE_INFO};
        SessionCI.next     = pBinding->GetConstDataPtr();
        SessionCI.systemId = SystemId;
        const XrResult res = xrCreateSession(Instance, &SessionCI, &XrS.Session);
        ASSERT_EQ(res, XR_SUCCESS) << "xrCreateSession failed";
    }

    {
        XrReferenceSpaceCreateInfo SpaceCI{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        SpaceCI.referenceSpaceType   = XR_REFERENCE_SPACE_TYPE_LOCAL;
        SpaceCI.poseInReferenceSpace = {{0, 0, 0, 1}, {0, 0, 0}};
        ASSERT_EQ(xrCreateReferenceSpace(XrS.Session, &SpaceCI, &XrS.Space), XR_SUCCESS);
    }

    Uint32 ViewCount = 0;
    xrEnumerateViewConfigurationViews(Instance, SystemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &ViewCount, nullptr);
    std::vector<XrViewConfigurationView> ConfigViews(ViewCount, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
    xrEnumerateViewConfigurationViews(Instance, SystemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, ViewCount, &ViewCount, ConfigViews.data());
    ASSERT_GT(ViewCount, 0u);

    // Swapchain in the first 8-bit RGBA format that the runtime can create (runtimes list their preferred
    // formats first). A runtime may offer a format and still fail to create it, e.g. when its compositor
    // can not import the images; every attempt is reported.
    const Uint32   Width           = ConfigViews[0].recommendedImageRectWidth;
    const Uint32   Height          = ConfigViews[0].recommendedImageRectHeight;
    TEXTURE_FORMAT SwapchainFormat = TEX_FORMAT_UNKNOWN;
    {
        Uint32 FormatCount = 0;
        xrEnumerateSwapchainFormats(XrS.Session, 0, &FormatCount, nullptr);
        std::vector<int64_t> Formats(FormatCount);
        xrEnumerateSwapchainFormats(XrS.Session, FormatCount, &FormatCount, Formats.data());

        const XrSwapchainUsageFlags UsageSets[] = {
            XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT,
            XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT,
        };

        std::string Attempts;
        bool        OnlyRuntimeFailures = true;
        for (size_t f = 0; f < Formats.size() && XrS.Swapchain == XR_NULL_HANDLE; ++f)
        {
            const TEXTURE_FORMAT Fmt = GetTextureFormatFromNative(Formats[f], DeviceType);
            if (Fmt != TEX_FORMAT_RGBA8_UNORM_SRGB && Fmt != TEX_FORMAT_BGRA8_UNORM_SRGB &&
                Fmt != TEX_FORMAT_RGBA8_UNORM && Fmt != TEX_FORMAT_BGRA8_UNORM)
                continue;

            for (XrSwapchainUsageFlags Usage : UsageSets)
            {
                XrSwapchainCreateInfo SwapchainCI{XR_TYPE_SWAPCHAIN_CREATE_INFO};
                SwapchainCI.usageFlags  = Usage;
                SwapchainCI.format      = Formats[f];
                SwapchainCI.sampleCount = 1;
                SwapchainCI.width       = Width;
                SwapchainCI.height      = Height;
                SwapchainCI.faceCount   = 1;
                SwapchainCI.arraySize   = 1;
                SwapchainCI.mipCount    = 1;

                const XrResult res = xrCreateSwapchain(XrS.Session, &SwapchainCI, &XrS.Swapchain);
                Attempts += std::string{"\n      "} + GetTextureFormatAttribs(Fmt).Name +
                    ((Usage & XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT) ? " color+transfer_dst: " : " color: ") + std::to_string(res);
                if (XR_SUCCEEDED(res))
                {
                    SwapchainFormat = Fmt;
                    break;
                }
                OnlyRuntimeFailures = OnlyRuntimeFailures && res == XR_ERROR_RUNTIME_FAILURE;
                XrS.Swapchain       = XR_NULL_HANDLE;
            }
        }
        std::cout << "    xrCreateSwapchain attempts (format, usage: XrResult):" << Attempts << std::endl;

        // The application's part is done: the session exists. XR_ERROR_RUNTIME_FAILURE for every format means
        // that the runtime itself can not provide images for this graphics API (e.g. Monado's Vulkan compositor
        // can not import D3D11 textures on drivers without VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT).
        if (XrS.Swapchain == XR_NULL_HANDLE && OnlyRuntimeFailures && !Attempts.empty())
        {
            GTEST_SKIP() << "The session was created, but the runtime failed to create swapchain images in every format "
                            "(XR_ERROR_RUNTIME_FAILURE); see the runtime's log";
        }
    }
    ASSERT_NE(XrS.Swapchain, XR_NULL_HANDLE) << "The runtime could not create a swapchain in any 8-bit RGBA format";

    Uint32 ImageCount = 0;
    xrEnumerateSwapchainImages(XrS.Swapchain, 0, &ImageCount, nullptr);
    ASSERT_GT(ImageCount, 0u);
    RefCntAutoPtr<IDataBlob> pImageData;
    AllocateOpenXRSwapchainImageData(DeviceType, ImageCount, &pImageData);
    ASSERT_NE(pImageData, nullptr);
    ASSERT_EQ(xrEnumerateSwapchainImages(XrS.Swapchain, ImageCount, &ImageCount, pImageData->GetDataPtr<XrSwapchainImageBaseHeader>()), XR_SUCCESS);

    // The images are rendered by pContext, which need not be context 0
    const Uint64 ContextMask = Uint64{1} << pContext->GetDesc().ContextId;

    TextureDesc ImageDesc;
    ImageDesc.Name                 = "OpenXR swapchain image";
    ImageDesc.Type                 = RESOURCE_DIM_TEX_2D;
    ImageDesc.Width                = Width;
    ImageDesc.Height               = Height;
    ImageDesc.Format               = SwapchainFormat;
    ImageDesc.BindFlags            = BIND_RENDER_TARGET;
    ImageDesc.ImmediateContextMask = ContextMask;

    std::vector<RefCntAutoPtr<ITexture>>     Images(ImageCount);
    std::vector<RefCntAutoPtr<ITextureView>> RTVs(ImageCount);
    for (Uint32 i = 0; i < ImageCount; ++i)
    {
        GetOpenXRSwapchainImage(pDevice, pImageData->GetConstDataPtr<XrSwapchainImageBaseHeader>(), i, ImageDesc, &Images[i]);
        ASSERT_NE(Images[i], nullptr);
        EXPECT_EQ(Images[i]->GetDesc().ImmediateContextMask, ContextMask) << "The swapchain image can not be used in context " << Uint32{pContext->GetDesc().ContextId};

        // Direct3D runtimes may create typeless images: the view gives the format the runtime expects
        TextureViewDesc RTVDesc;
        RTVDesc.ViewType = TEXTURE_VIEW_RENDER_TARGET;
        RTVDesc.Format   = SwapchainFormat;
        Images[i]->CreateView(RTVDesc, &RTVs[i]);
        ASSERT_NE(RTVs[i], nullptr);
    }
    std::cout << "    Swapchain: " << ImageCount << " images, " << Width << "x" << Height << ", " << GetTextureFormatAttribs(SwapchainFormat).Name
              << " (image format " << GetTextureFormatAttribs(Images[0]->GetDesc().Format).Name << ")" << std::endl;

    // Frame loop: begin the session when it is ready, then render and submit frames
    Uint32     FramesSubmitted = 0;
    bool       SessionRunning  = false;
    const auto StartTime       = std::chrono::steady_clock::now();
    while (FramesSubmitted < FramesToSubmit && std::chrono::steady_clock::now() - StartTime < std::chrono::seconds{20})
    {
        XrEventDataBuffer Event{XR_TYPE_EVENT_DATA_BUFFER};
        while (xrPollEvent(Instance, &Event) == XR_SUCCESS)
        {
            if (Event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
            {
                const XrSessionState State = reinterpret_cast<const XrEventDataSessionStateChanged*>(&Event)->state;
                if (State == XR_SESSION_STATE_READY)
                {
                    XrSessionBeginInfo BeginInfo{XR_TYPE_SESSION_BEGIN_INFO};
                    BeginInfo.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                    ASSERT_EQ(xrBeginSession(XrS.Session, &BeginInfo), XR_SUCCESS);
                    SessionRunning = true;
                }
                else if (State == XR_SESSION_STATE_STOPPING || State == XR_SESSION_STATE_EXITING || State == XR_SESSION_STATE_LOSS_PENDING)
                {
                    SessionRunning = false;
                }
            }
            Event = {XR_TYPE_EVENT_DATA_BUFFER};
        }

        if (!SessionRunning)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
            continue;
        }

        XrFrameState FrameState{XR_TYPE_FRAME_STATE};
        ASSERT_EQ(xrWaitFrame(XrS.Session, nullptr, &FrameState), XR_SUCCESS);
        ASSERT_EQ(xrBeginFrame(XrS.Session, nullptr), XR_SUCCESS);

        std::vector<XrCompositionLayerProjectionView>    ProjViews;
        XrCompositionLayerProjection                     Layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        std::vector<const XrCompositionLayerBaseHeader*> Layers;
        if (FrameState.shouldRender)
        {
            XrViewLocateInfo LocateInfo{XR_TYPE_VIEW_LOCATE_INFO};
            LocateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            LocateInfo.displayTime           = FrameState.predictedDisplayTime;
            LocateInfo.space                 = XrS.Space;
            XrViewState         ViewState{XR_TYPE_VIEW_STATE};
            std::vector<XrView> Views(ViewCount, {XR_TYPE_VIEW});
            Uint32              LocatedCount = 0;
            ASSERT_EQ(xrLocateViews(XrS.Session, &LocateInfo, &ViewState, ViewCount, &LocatedCount, Views.data()), XR_SUCCESS);

            Uint32 ImageIndex = 0;
            ASSERT_EQ(xrAcquireSwapchainImage(XrS.Swapchain, nullptr, &ImageIndex), XR_SUCCESS);
            XrSwapchainImageWaitInfo WaitInfo{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
            WaitInfo.timeout = XR_INFINITE_DURATION;
            ASSERT_EQ(xrWaitSwapchainImage(XrS.Swapchain, &WaitInfo), XR_SUCCESS);

            // The runtime gives the image in the color attachment state and expects it back in that state
            ITexture*     pImage       = Images[ImageIndex];
            ITextureView* pRTV         = RTVs[ImageIndex];
            const float   ClearColor[] = {0.1f, 0.2f + 0.02f * (FramesSubmitted % 20), 0.6f, 1.f};
            pImage->SetState(RESOURCE_STATE_RENDER_TARGET);
            pContext->SetRenderTargets(1, &pRTV, nullptr, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
            pContext->ClearRenderTarget(pRTV, ClearColor, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
            pContext->SetRenderTargets(0, nullptr, nullptr, RESOURCE_STATE_TRANSITION_MODE_NONE);
            StateTransitionDesc ToRT{pImage, RESOURCE_STATE_UNKNOWN, RESOURCE_STATE_RENDER_TARGET, STATE_TRANSITION_FLAG_UPDATE_STATE};
            pContext->TransitionResourceStates(1, &ToRT);
            // The runtime's work on the queue must follow the rendering
            pContext->Flush();

            XrSwapchainImageReleaseInfo ReleaseInfo{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            ASSERT_EQ(xrReleaseSwapchainImage(XrS.Swapchain, &ReleaseInfo), XR_SUCCESS);

            ProjViews.resize(LocatedCount, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW});
            for (Uint32 v = 0; v < LocatedCount; ++v)
            {
                ProjViews[v].pose                      = Views[v].pose;
                ProjViews[v].fov                       = Views[v].fov;
                ProjViews[v].subImage.swapchain        = XrS.Swapchain;
                ProjViews[v].subImage.imageRect.offset = {0, 0};
                ProjViews[v].subImage.imageRect.extent = {static_cast<int32_t>(Width), static_cast<int32_t>(Height)};
            }
            Layer.space     = XrS.Space;
            Layer.viewCount = LocatedCount;
            Layer.views     = ProjViews.data();
            Layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&Layer));
        }

        XrFrameEndInfo EndInfo{XR_TYPE_FRAME_END_INFO};
        EndInfo.displayTime          = FrameState.predictedDisplayTime;
        EndInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        EndInfo.layerCount           = static_cast<Uint32>(Layers.size());
        EndInfo.layers               = Layers.empty() ? nullptr : Layers.data();
        ASSERT_EQ(xrEndFrame(XrS.Session, &EndInfo), XR_SUCCESS);

        if (!Layers.empty())
            ++FramesSubmitted;
        pContext->FinishFrame();
    }

    std::cout << "    Submitted " << FramesSubmitted << " frames" << std::endl;
    EXPECT_EQ(FramesSubmitted, FramesToSubmit) << "The session did not run";

    pContext->WaitForIdle();

    // xrEndSession is only valid in the STOPPING state, which the runtime enters after xrRequestExitSession
    if (SessionRunning)
    {
        EXPECT_EQ(xrRequestExitSession(XrS.Session), XR_SUCCESS);
        const bool Stopping = WaitForSessionState(Instance, XR_SESSION_STATE_STOPPING, std::chrono::seconds{5});
        EXPECT_TRUE(Stopping) << "The session did not reach the STOPPING state";
        if (Stopping)
            EXPECT_EQ(xrEndSession(XrS.Session), XR_SUCCESS);
    }

    RTVs.clear();
    Images.clear();
}

} // namespace Testing

} // namespace Diligent
