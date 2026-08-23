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

// Runs an OpenXR session on a Direct3D11 device created on the adapter that the runtime requires
// (XR_KHR_D3D11_enable). See OpenXRTestHelpers.hpp for how the runtime is found.

#include "WinHPreface.h"
#include <d3d11.h>
#include <dxgi.h>
#include "WinHPostface.h"

#include "GPUTestingEnvironment.hpp"
#include "OpenXRTestHelpers.hpp"

#include "RenderDeviceD3D11.h"
#include "EngineFactoryD3D11.h"
#include "OpenXRUtilities.h"

#define XR_USE_GRAPHICS_API_D3D11
#include <openxr/openxr_platform.h>

#include <iostream>
#include <stdexcept>

#include "gtest/gtest.h"

using namespace Diligent;
using namespace Diligent::Testing;

namespace
{

LUID GetAdapterLuid(ID3D11Device* pd3d11Device)
{
    LUID Luid{};

    IDXGIDevice* pDXGIDevice = nullptr;
    if (SUCCEEDED(pd3d11Device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&pDXGIDevice))))
    {
        IDXGIAdapter* pAdapter = nullptr;
        if (SUCCEEDED(pDXGIDevice->GetAdapter(&pAdapter)))
        {
            DXGI_ADAPTER_DESC Desc{};
            if (SUCCEEDED(pAdapter->GetDesc(&Desc)))
                Luid = Desc.AdapterLuid;
            pAdapter->Release();
        }
        pDXGIDevice->Release();
    }
    return Luid;
}

TEST(OpenXRD3D11, Session)
{
    GPUTestingEnvironment* pEnv = GPUTestingEnvironment::GetInstance();
    if (pEnv->GetDevice()->GetDeviceInfo().Type != RENDER_DEVICE_TYPE_D3D11)
        GTEST_SKIP() << "Direct3D11 only";

    GPUTestingEnvironment::ScopedReset EnvironmentAutoReset;

    OpenXRTestInstance Xr;
    {
        const std::string SkipReason = Xr.Create(XR_KHR_D3D11_ENABLE_EXTENSION_NAME);
        if (!SkipReason.empty())
            GTEST_SKIP() << SkipReason;
    }

    // The adapter and the minimum feature level that the runtime requires
    XrGraphicsRequirementsD3D11KHR Requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
    {
        PFN_xrGetD3D11GraphicsRequirementsKHR xrGetD3D11GraphicsRequirementsKHR =
            reinterpret_cast<PFN_xrGetD3D11GraphicsRequirementsKHR>(Xr.GetProcAddr("xrGetD3D11GraphicsRequirementsKHR"));
        ASSERT_NE(xrGetD3D11GraphicsRequirementsKHR, nullptr);
        ASSERT_EQ(xrGetD3D11GraphicsRequirementsKHR(Xr.GetInstance(), Xr.GetSystemId(), &Requirements), XR_SUCCESS);
    }

    RefCntAutoPtr<IEngineFactoryD3D11> pFactory{pEnv->GetDevice()->GetEngineFactory(), IID_EngineFactoryD3D11};
    ASSERT_NE(pFactory, nullptr);

    const OpenXRAttribs XRAttribs = Xr.GetAttribs();

    // Direct3D11 has a single immediate context
    EngineD3D11CreateInfo EngineCI;
    EngineCI.pXRAttribs = &XRAttribs;

    RefCntAutoPtr<IRenderDevice>  pDevice;
    RefCntAutoPtr<IDeviceContext> pContext;
    try
    {
        // CreateDeviceAndContextsD3D11 throws when it can not use the runtime's adapter
        pFactory->CreateDeviceAndContextsD3D11(EngineCI, &pDevice, &pContext);
    }
    catch (const std::exception& e)
    {
        FAIL() << "CreateDeviceAndContextsD3D11 failed: " << e.what();
    }
    ASSERT_NE(pDevice, nullptr);
    ASSERT_NE(pContext, nullptr);

    RefCntAutoPtr<IRenderDeviceD3D11> pDeviceD3D11{pDevice, IID_RenderDeviceD3D11};
    ASSERT_NE(pDeviceD3D11, nullptr);
    ID3D11Device* pd3d11Device = pDeviceD3D11->GetD3D11Device();

    // The device is on the runtime's adapter, with at least the required feature level
    {
        const LUID DeviceLuid = GetAdapterLuid(pd3d11Device);
        std::cout << "    Device selected by the runtime: " << pDevice->GetAdapterInfo().Description
                  << ", LUID " << std::hex << DeviceLuid.HighPart << ':' << DeviceLuid.LowPart
                  << " (runtime LUID " << Requirements.adapterLuid.HighPart << ':' << Requirements.adapterLuid.LowPart << ")"
                  << ", feature level 0x" << pd3d11Device->GetFeatureLevel()
                  << " (runtime minimum 0x" << Requirements.minFeatureLevel << ")" << std::dec << std::endl;
        EXPECT_EQ(DeviceLuid.LowPart, Requirements.adapterLuid.LowPart);
        EXPECT_EQ(DeviceLuid.HighPart, Requirements.adapterLuid.HighPart);
        EXPECT_GE(pd3d11Device->GetFeatureLevel(), Requirements.minFeatureLevel);
    }

    // The graphics binding names the device
    {
        RefCntAutoPtr<IDataBlob> pBinding;
        GetOpenXRGraphicsBinding(pDevice, pContext, &pBinding);
        ASSERT_NE(pBinding, nullptr);
        const XrGraphicsBindingD3D11KHR& Binding = *pBinding->GetConstDataPtr<XrGraphicsBindingD3D11KHR>();
        EXPECT_EQ(Binding.type, XR_TYPE_GRAPHICS_BINDING_D3D11_KHR);
        EXPECT_EQ(Binding.device, pd3d11Device);
    }

    RunOpenXRFrames(Xr, pDevice, pContext, 30);
}

} // namespace
