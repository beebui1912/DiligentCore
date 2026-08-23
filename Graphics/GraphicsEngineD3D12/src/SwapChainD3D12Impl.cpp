/*
 *  Copyright 2019-2025 Diligent Graphics LLC
 *  Copyright 2015-2019 Egor Yusov
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

#include "pch.h"

#include <algorithm>

#include "SwapChainD3D12Impl.hpp"

#include "RenderDeviceD3D12Impl.hpp"
#include "DeviceContextD3D12Impl.hpp"
#include "TextureD3D12Impl.hpp"

#include "DXGITypeConversions.hpp"
#include "EngineMemory.h"

namespace Diligent
{

SwapChainD3D12Impl::SwapChainD3D12Impl(IReferenceCounters*       pRefCounters,
                                       const SwapChainDesc&      SCDesc,
                                       const FullScreenModeDesc& FSDesc,
                                       RenderDeviceD3D12Impl*    pRenderDeviceD3D12,
                                       DeviceContextD3D12Impl*   pDeviceContextD3D12,
                                       const NativeWindow&       Window) :
    // clang-format off
    TSwapChainBase
    {
        pRefCounters,
        pRenderDeviceD3D12,
        pDeviceContextD3D12,
        SCDesc,
        FSDesc,
        Window
    },
    m_pBackBufferRTV(STD_ALLOCATOR_RAW_MEM(RefCntAutoPtr<ITextureView>, GetRawAllocator(), "Allocator for vector<RefCntAutoPtr<ITextureView>>"))
// clang-format on
{
    InitAlternateFrameContexts(pDeviceContextD3D12);

    pRenderDeviceD3D12->LockCmdQueueAndRun(
        pDeviceContextD3D12->GetCommandQueueId(),
        [this](ICommandQueueD3D12* pCmdQueue) //
        {
            CreateDXGISwapChain(pCmdQueue->GetD3D12CommandQueue());
        } //
    );
    if (!m_AlternateFrameContexts.empty())
        AssignBuffersToAlternateFrameContexts();

    InitBuffersAndViews();
}

SwapChainD3D12Impl::~SwapChainD3D12Impl()
{
}

void SwapChainD3D12Impl::InitAlternateFrameContexts(const DeviceContextD3D12Impl* pSwapChainContext)
{
    const Uint64 ContextMask = m_SwapChainDesc.AlternateFrameContextMask;
    if (ContextMask == 0)
        return;

    RenderDeviceD3D12Impl* pDeviceD3D12 = m_pRenderDevice.RawPtr<RenderDeviceD3D12Impl>();
    if (pDeviceD3D12->GetNodeCount() < 2)
    {
        LOG_WARNING_MESSAGE("SwapChainDesc.AlternateFrameContextMask is ignored: the device does not use linked nodes.");
        m_SwapChainDesc.AlternateFrameContextMask = 0;
        return;
    }

    Uint32 UsedNodes = 0;
    for (Uint32 CtxId = 0; CtxId < 64; ++CtxId)
    {
        if ((ContextMask & (Uint64{1} << CtxId)) == 0)
            continue;

        RefCntAutoPtr<DeviceContextD3D12Impl> pContext = pDeviceD3D12->GetImmediateContext(CtxId);
        if (!pContext)
            LOG_ERROR_AND_THROW("SwapChainDesc.AlternateFrameContextMask refers to immediate context ", CtxId, ", which does not exist.");

        const DeviceContextDesc& CtxDesc = pContext->GetDesc();
        if ((CtxDesc.QueueType & COMMAND_QUEUE_TYPE_GRAPHICS) != COMMAND_QUEUE_TYPE_GRAPHICS)
            LOG_ERROR_AND_THROW("SwapChainDesc.AlternateFrameContextMask: immediate context ", CtxId, " does not use a graphics queue.");

        const Uint32 NodeBit = 1u << CtxDesc.NodeIndex;
        if ((UsedNodes & NodeBit) != 0)
            LOG_ERROR_AND_THROW("SwapChainDesc.AlternateFrameContextMask: more than one immediate context on node ", Uint32{CtxDesc.NodeIndex}, ".");
        UsedNodes |= NodeBit;

        m_AlternateFrameContexts.push_back(CtxId);
    }

    if ((ContextMask & (Uint64{1} << pSwapChainContext->GetDesc().ContextId)) == 0)
        LOG_ERROR_AND_THROW("SwapChainDesc.AlternateFrameContextMask must include the immediate context the swap chain is created with.");

    if (m_SwapChainDesc.BufferCount < m_AlternateFrameContexts.size())
    {
        LOG_ERROR_AND_THROW("SwapChainDesc.BufferCount (", m_SwapChainDesc.BufferCount, ") must not be less than the number of contexts in AlternateFrameContextMask (",
                            m_AlternateFrameContexts.size(), ").");
    }
}

void SwapChainD3D12Impl::AssignBuffersToAlternateFrameContexts()
{
    RenderDeviceD3D12Impl* pDeviceD3D12 = m_pRenderDevice.RawPtr<RenderDeviceD3D12Impl>();

    std::vector<UINT>      NodeMasks(m_SwapChainDesc.BufferCount);
    std::vector<IUnknown*> Queues(m_SwapChainDesc.BufferCount);
    for (Uint32 backbuff = 0; backbuff < m_SwapChainDesc.BufferCount; ++backbuff)
    {
        const Uint32 CtxId = m_AlternateFrameContexts[backbuff % m_AlternateFrameContexts.size()];

        RefCntAutoPtr<IDeviceContext> pContext = GetBackBufferContext(backbuff);
        if (!pContext)
            LOG_ERROR_AND_THROW("Immediate context ", CtxId, " of SwapChainDesc.AlternateFrameContextMask has been released.");

        NodeMasks[backbuff] = 1u << pContext->GetDesc().NodeIndex;
        pDeviceD3D12->LockCmdQueueAndRun(
            SoftwareQueueIndex{CtxId},
            [&Queue = Queues[backbuff]](ICommandQueueD3D12* pCmdQueue) //
            {
                Queue = pCmdQueue->GetD3D12CommandQueue();
            } //
        );
    }

    DXGI_SWAP_CHAIN_DESC SCDes{};
    m_pSwapChain->GetDesc(&SCDes);
    CHECK_D3D_RESULT_THROW(m_pSwapChain->ResizeBuffers1(SCDes.BufferCount, m_SwapChainDesc.Width, m_SwapChainDesc.Height, SCDes.BufferDesc.Format, SCDes.Flags,
                                                        NodeMasks.data(), Queues.data()),
                           "Failed to assign the swap chain buffers to the nodes of AlternateFrameContextMask");
}

RefCntAutoPtr<IDeviceContext> SwapChainD3D12Impl::GetBackBufferContext(Uint32 BackBuffer)
{
    if (m_AlternateFrameContexts.empty())
        return m_wpDeviceContext.Lock();

    return m_pRenderDevice.RawPtr<RenderDeviceD3D12Impl>()->GetImmediateContext(m_AlternateFrameContexts[BackBuffer % m_AlternateFrameContexts.size()]);
}

void SwapChainD3D12Impl::InitBuffersAndViews()
{
    m_pBackBufferRTV.resize(m_SwapChainDesc.BufferCount);
    for (Uint32 backbuff = 0; backbuff < m_SwapChainDesc.BufferCount; ++backbuff)
    {
        // DXGI creates a back buffer on the node of its present queue, visible to that node only.
        // The context's node is the node of its queue (see AttachToD3D12Device()).
        UINT   BackBufferNodeMask = 1;
        Uint64 BackBufferCtxMask  = 1;
        if (RefCntAutoPtr<IDeviceContext> pContext = GetBackBufferContext(backbuff))
        {
            BackBufferNodeMask = 1u << pContext->GetDesc().NodeIndex;
            BackBufferCtxMask  = Uint64{1} << pContext->GetDesc().ContextId;
        }

        CComPtr<ID3D12Resource> pBackBuffer;

        HRESULT hr = m_pSwapChain->GetBuffer(backbuff, __uuidof(pBackBuffer), reinterpret_cast<void**>(static_cast<ID3D12Resource**>(&pBackBuffer)));
        if (FAILED(hr))
            LOG_ERROR_AND_THROW("Failed to get back buffer ", backbuff, " from the swap chain");

        hr = pBackBuffer->SetName(L"Main back buffer");
        VERIFY_EXPR(SUCCEEDED(hr));

        TextureDesc BackBufferDesc;
        String      Name = "Main back buffer ";
        Name += std::to_string(backbuff);
        BackBufferDesc.Name = Name.c_str();
        // Linked multi-GPU: frames rendered on other nodes must be copied to this node before being presented
        BackBufferDesc.CreationNodeMask     = BackBufferNodeMask;
        BackBufferDesc.VisibleNodeMask      = BackBufferNodeMask;
        BackBufferDesc.ImmediateContextMask = BackBufferCtxMask;

        RefCntAutoPtr<TextureD3D12Impl> pBackBufferTex;
        m_pRenderDevice.RawPtr<RenderDeviceD3D12Impl>()->CreateTexture(BackBufferDesc, pBackBuffer, RESOURCE_STATE_UNDEFINED, &pBackBufferTex);
        TextureViewDesc RTVDesc;
        RTVDesc.ViewType = TEXTURE_VIEW_RENDER_TARGET;
        RTVDesc.Format   = m_SwapChainDesc.ColorBufferFormat;
        RefCntAutoPtr<ITextureView> pRTV;
        pBackBufferTex->CreateView(RTVDesc, &pRTV);
        m_pBackBufferRTV[backbuff] = RefCntAutoPtr<ITextureViewD3D12>(pRTV, IID_TextureViewD3D12);
    }

    if (m_SwapChainDesc.DepthBufferFormat != TEX_FORMAT_UNKNOWN)
    {
        // The depth buffer is on the node of the back buffers that are rendered with it
        const Uint32 NumDepthBuffers = std::max(static_cast<Uint32>(m_AlternateFrameContexts.size()), 1u);
        m_pDepthBufferDSV.resize(NumDepthBuffers);
        for (Uint32 depthbuff = 0; depthbuff < NumDepthBuffers; ++depthbuff)
        {
            TextureDesc DepthBufferDesc;
            DepthBufferDesc.Type        = RESOURCE_DIM_TEX_2D;
            DepthBufferDesc.Width       = m_SwapChainDesc.Width;
            DepthBufferDesc.Height      = m_SwapChainDesc.Height;
            DepthBufferDesc.Format      = m_SwapChainDesc.DepthBufferFormat;
            DepthBufferDesc.SampleCount = 1;
            DepthBufferDesc.Usage       = USAGE_DEFAULT;
            DepthBufferDesc.BindFlags   = BIND_DEPTH_STENCIL;

            if (RefCntAutoPtr<IDeviceContext> pContext = GetBackBufferContext(depthbuff))
            {
                DepthBufferDesc.CreationNodeMask     = 1u << pContext->GetDesc().NodeIndex;
                DepthBufferDesc.VisibleNodeMask      = DepthBufferDesc.CreationNodeMask;
                DepthBufferDesc.ImmediateContextMask = Uint64{1} << pContext->GetDesc().ContextId;
            }

            DepthBufferDesc.ClearValue.Format               = GetDefaultTextureViewFormat(DepthBufferDesc.Format, TEXTURE_VIEW_DEPTH_STENCIL, DepthBufferDesc.BindFlags);
            DepthBufferDesc.ClearValue.DepthStencil.Depth   = m_SwapChainDesc.DefaultDepthValue;
            DepthBufferDesc.ClearValue.DepthStencil.Stencil = m_SwapChainDesc.DefaultStencilValue;
            DepthBufferDesc.Name                            = "Main depth buffer";
            RefCntAutoPtr<ITexture> pDepthBufferTex;
            m_pRenderDevice->CreateTexture(DepthBufferDesc, nullptr, static_cast<ITexture**>(&pDepthBufferTex));
            ITextureView* pDSV           = pDepthBufferTex->GetDefaultView(TEXTURE_VIEW_DEPTH_STENCIL);
            m_pDepthBufferDSV[depthbuff] = RefCntAutoPtr<ITextureViewD3D12>(pDSV, IID_TextureViewD3D12);
        }
    }
}

void SwapChainD3D12Impl::Present(Uint32 SyncInterval)
{
#if PLATFORM_UNIVERSAL_WINDOWS
    SyncInterval = 1; // Interval 0 is not supported on Windows Phone
#endif

    // Alternate frame rendering: DXGI presents the current back buffer on the queue of its node
    RefCntAutoPtr<IDeviceContext> pDeviceContext = GetBackBufferContext(m_pSwapChain->GetCurrentBackBufferIndex());
    if (!pDeviceContext)
    {
        LOG_ERROR_MESSAGE("Immediate context has been released");
        return;
    }

    DeviceContextD3D12Impl* pImmediateCtxD3D12 = pDeviceContext.RawPtr<DeviceContextD3D12Impl>();

    CommandContext&   CmdCtx      = pImmediateCtxD3D12->GetCmdContext();
    TextureD3D12Impl* pBackBuffer = ClassPtrCast<TextureD3D12Impl>(GetCurrentBackBufferRTV()->GetTexture());

    // A successful Present call for DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL SwapChains unbinds
    // backbuffer 0 from all GPU writeable bind points.
    pImmediateCtxD3D12->UnbindTextureFromFramebuffer(pBackBuffer, false);

    CmdCtx.TransitionResource(*pBackBuffer, RESOURCE_STATE_PRESENT);

    pImmediateCtxD3D12->Flush();

    // In contrast to MSDN sample, we wait for the frame as late as possible - right
    // before presenting.
    // https://docs.microsoft.com/en-us/windows/uwp/gaming/reduce-latency-with-dxgi-1-3-swap-chains#step-4-wait-before-rendering-each-frame
    WaitForFrame();

    HRESULT hr = PresentInternal(SyncInterval);
    VERIFY(SUCCEEDED(hr), "Present failed");

    if (m_SwapChainDesc.IsPrimary)
    {
        pImmediateCtxD3D12->FinishFrame();
        RenderDeviceD3D12Impl* pDeviceD3D12 = ClassPtrCast<RenderDeviceD3D12Impl>(pImmediateCtxD3D12->GetDevice());
        pDeviceD3D12->ReleaseStaleResources();
    }

    // A successful Present call for DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL SwapChains unbinds
    // backbuffer 0 from all GPU writeable bind points.
}

void SwapChainD3D12Impl::UpdateSwapChain(bool CreateNew)
{
    // When switching to full screen mode, WM_SIZE is send to the window
    // and Resize() is called before the new swap chain is created
    if (!m_pSwapChain)
        return;

    RefCntAutoPtr<IDeviceContext> pDeviceContext = m_wpDeviceContext.Lock();
    VERIFY(pDeviceContext, "Immediate context has been released");
    if (pDeviceContext)
    {
        RenderDeviceD3D12Impl* pDeviceD3D12 = m_pRenderDevice.RawPtr<RenderDeviceD3D12Impl>();

        // The contexts that render to the back buffers
        std::vector<RefCntAutoPtr<IDeviceContext>> Contexts;
        if (m_AlternateFrameContexts.empty())
        {
            Contexts.emplace_back(pDeviceContext);
        }
        else
        {
            for (Uint32 CtxId : m_AlternateFrameContexts)
            {
                RefCntAutoPtr<IDeviceContext> pContext{pDeviceD3D12->GetImmediateContext(CtxId)};
                if (pContext)
                    Contexts.emplace_back(std::move(pContext));
            }
        }

        for (RefCntAutoPtr<IDeviceContext>& pContext : Contexts)
            pContext->Flush();

        try
        {
            for (RefCntAutoPtr<IDeviceContext>& pContext : Contexts)
            {
                DeviceContextD3D12Impl* pImmediateCtxD3D12 = pContext.RawPtr<DeviceContextD3D12Impl>();
                bool                    RenderTargetsReset = false;
                for (Uint32 i = 0; i < m_pBackBufferRTV.size() && !RenderTargetsReset; ++i)
                {
                    TextureD3D12Impl* pCurrentBackBuffer = ClassPtrCast<TextureD3D12Impl>(m_pBackBufferRTV[i]->GetTexture());
                    RenderTargetsReset                   = pImmediateCtxD3D12->UnbindTextureFromFramebuffer(pCurrentBackBuffer, false);
                }

                if (RenderTargetsReset)
                {
                    LOG_INFO_MESSAGE_ONCE("Resizing the swap chain requires back and depth-stencil buffers to be unbound from the device context. "
                                          "An application should use SetRenderTargets() to restore them.");
                }
            }

            // All references to the swap chain must be released before it can be resized
            m_pBackBufferRTV.clear();
            m_pDepthBufferDSV.clear();

            // This will release references to D3D12 swap chain buffers hold by
            // m_pBackBufferRTV[]
            pDeviceD3D12->IdleGPU();

            if (CreateNew)
            {
                m_pSwapChain.Release();
                m_pRenderDevice.RawPtr<RenderDeviceD3D12Impl>()->LockCmdQueueAndRun(
                    pDeviceContext.RawPtr<DeviceContextD3D12Impl>()->GetCommandQueueId(),
                    [this](ICommandQueueD3D12* pCmdQueue) //
                    {
                        CreateDXGISwapChain(pCmdQueue->GetD3D12CommandQueue());
                    } //
                );
                if (!m_AlternateFrameContexts.empty())
                    AssignBuffersToAlternateFrameContexts();
            }
            else if (!m_AlternateFrameContexts.empty())
            {
                AssignBuffersToAlternateFrameContexts();
            }
            else
            {
                DXGI_SWAP_CHAIN_DESC SCDes;
                memset(&SCDes, 0, sizeof(SCDes));
                m_pSwapChain->GetDesc(&SCDes);
                CHECK_D3D_RESULT_THROW(m_pSwapChain->ResizeBuffers(SCDes.BufferCount, m_SwapChainDesc.Width,
                                                                   m_SwapChainDesc.Height, SCDes.BufferDesc.Format,
                                                                   SCDes.Flags),
                                       "Failed to resize the DXGI swap chain");
            }

            InitBuffersAndViews();
        }
        catch (const std::runtime_error&)
        {
            LOG_ERROR("Failed to resize the swap chain");
        }
    }
}

void SwapChainD3D12Impl::Resize(Uint32 NewWidth, Uint32 NewHeight, SURFACE_TRANSFORM NewPreTransform)
{
    if (TSwapChainBase::Resize(NewWidth, NewHeight, NewPreTransform))
    {
        UpdateSwapChain(false);
    }
}

} // namespace Diligent
