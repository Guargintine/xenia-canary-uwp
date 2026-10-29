/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/d3d12/d3d12_provider.h"

#include <algorithm>
#include <cstdlib>
#include <string>
#include <string_view>

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/base/string.h"
#include "xenia/ui/d3d12/d3d12_immediate_drawer.h"
#include "xenia/ui/d3d12/d3d12_presenter.h"
#include "xenia/ui/d3d12/d3d12_util.h"
DEFINE_bool(d3d12_debug, false, "Enable Direct3D 12 and DXGI debug layer.",
            "D3D12");
DEFINE_bool(
    d3d12_dred, XE_PLATFORM_WINRT,
    "Record Device Removed Extended Data: if the Direct3D 12 device is lost, "
    "the log gets the GPU operations that did not complete and the allocation "
    "at a faulting address. Costs a little GPU time.",
    "D3D12");
DEFINE_bool(d3d12_break_on_error, false,
            "Break on Direct3D 12 validation errors.", "D3D12");
DEFINE_bool(d3d12_break_on_warning, false,
            "Break on Direct3D 12 validation warnings.", "D3D12");
DEFINE_int32(d3d12_adapter, -1,
             "Index of the DXGI adapter to use. "
             "-1 for any physical adapter, -2 for WARP software rendering.",
             "D3D12");
DEFINE_int32(
    d3d12_queue_priority, 1,
    "Graphics (direct) command queue scheduling priority, 0 - normal, 1 - "
    "high, 2 - global realtime (requires administrator privileges, may impact "
    "system responsibility)",
    "D3D12");

namespace xe {
namespace ui {
namespace d3d12 {

bool D3D12Provider::IsD3D12APIAvailable() {
  HMODULE library_d3d12 = LoadLibraryW(L"D3D12.dll");
  if (!library_d3d12) {
    return false;
  }
  FreeLibrary(library_d3d12);
  return true;
}

const std::string& D3D12Provider::GetAdapterDescription() const {
  return adapter_description_;
}

// Check for Intel Arc cards and Intel Graphics iGPUs which use
// the same architecture.
bool D3D12Provider::IsIntelArcGpu() const {
  if (adapter_vendor_id_ != GpuVendorID::kIntel) {
    return false;
  }
  return adapter_description_.starts_with("Intel(R) Arc(TM)") ||
         adapter_description_.starts_with("Intel(R) Graphics");
}

std::unique_ptr<D3D12Provider> D3D12Provider::Create() {
  std::unique_ptr<D3D12Provider> provider(new D3D12Provider);
  if (!provider->Initialize()) {
    xe::FatalError(
        "Unable to initialize Direct3D 12 graphics subsystem.\n"
        "\n"
        "Ensure that you have the latest drivers for your GPU and it supports "
        "Direct3D 12 with the feature level of at least 11_0.\n"
        "\n"
        "See https://xenia.jp/faq/ for more information and a list of "
        "supported GPUs.");
    return nullptr;
  }

  return provider;
}

D3D12Provider::~D3D12Provider() {
  if (memory_info_adapter_ != nullptr) {
    xe::crash_report::RemoveStatusSource(DescribeMemoryStatus, this);
    memory_info_adapter_->Release();
  }
  if (graphics_analysis_ != nullptr) {
    graphics_analysis_->Release();
  }
  if (direct_queue_ != nullptr) {
    direct_queue_->Release();
  }
  if (device_ != nullptr) {
    device_->Release();
  }
  if (dxgi_factory_ != nullptr) {
    dxgi_factory_->Release();
  }

  if (cvars::d3d12_debug && pfn_dxgi_get_debug_interface1_) {
    Microsoft::WRL::ComPtr<IDXGIDebug> dxgi_debug;
    if (SUCCEEDED(
            pfn_dxgi_get_debug_interface1_(0, IID_PPV_ARGS(&dxgi_debug)))) {
      dxgi_debug->ReportLiveObjects(DXGI_DEBUG_ALL, DXGI_DEBUG_RLO_ALL);
    }
  }

  if (library_dxcompiler_ != nullptr) {
    FreeLibrary(library_dxcompiler_);
  }
  if (library_dxilconv_ != nullptr) {
    FreeLibrary(library_dxilconv_);
  }
  if (library_d3dcompiler_ != nullptr) {
    FreeLibrary(library_d3dcompiler_);
  }
  if (library_d3d12_ != nullptr) {
    FreeLibrary(library_d3d12_);
  }
  if (library_dxgi_ != nullptr) {
    FreeLibrary(library_dxgi_);
  }
}

bool D3D12Provider::EnableIncreaseBasePriorityPrivilege() {
  TOKEN_PRIVILEGES privileges;
  privileges.PrivilegeCount = 1;
  privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
  if (!LookupPrivilegeValue(nullptr, SE_INC_BASE_PRIORITY_NAME,
                            &privileges.Privileges[0].Luid)) {
    return false;
  }
  HANDLE token;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES, &token)) {
    return false;
  }
  bool enabled = AdjustTokenPrivileges(token, false, &privileges,
                                       sizeof(privileges), nullptr, nullptr) &&
                 GetLastError() != ERROR_NOT_ALL_ASSIGNED;
  CloseHandle(token);
  return enabled;
}

bool D3D12Provider::Initialize() {
  // Load the core libraries.
  library_dxgi_ = LoadLibraryW(L"dxgi.dll");
  library_d3d12_ = LoadLibraryW(L"D3D12.dll");
  if (!library_dxgi_ || !library_d3d12_) {
    XELOGE("Failed to load dxgi.dll or D3D12.dll");
    return false;
  }
  bool libraries_loaded = true;
  libraries_loaded &=
      (pfn_create_dxgi_factory2_ = PFNCreateDXGIFactory2(
           GetProcAddress(library_dxgi_, "CreateDXGIFactory2"))) != nullptr;
  libraries_loaded &=
      (pfn_dxgi_get_debug_interface1_ = PFNDXGIGetDebugInterface1(
           GetProcAddress(library_dxgi_, "DXGIGetDebugInterface1"))) != nullptr;
  libraries_loaded &=
      (pfn_d3d12_get_debug_interface_ = PFN_D3D12_GET_DEBUG_INTERFACE(
           GetProcAddress(library_d3d12_, "D3D12GetDebugInterface"))) !=
      nullptr;
  libraries_loaded &=
      (pfn_d3d12_create_device_ = PFN_D3D12_CREATE_DEVICE(
           GetProcAddress(library_d3d12_, "D3D12CreateDevice"))) != nullptr;
  libraries_loaded &=
      (pfn_d3d12_serialize_root_signature_ = PFN_D3D12_SERIALIZE_ROOT_SIGNATURE(
           GetProcAddress(library_d3d12_, "D3D12SerializeRootSignature"))) !=
      nullptr;
  if (!libraries_loaded) {
    XELOGE("Failed to get DXGI or Direct3D 12 functions");
    return false;
  }

  // Load optional D3DCompiler_47.dll.
  pfn_d3d_disassemble_ = nullptr;
  library_d3dcompiler_ = LoadLibraryW(L"D3DCompiler_47.dll");
  if (library_d3dcompiler_) {
    pfn_d3d_disassemble_ =
        pD3DDisassemble(GetProcAddress(library_d3dcompiler_, "D3DDisassemble"));
    if (pfn_d3d_disassemble_ == nullptr) {
      XELOGD(
          "Failed to get D3DDisassemble from D3DCompiler_47.dll, DXBC "
          "disassembly for debugging will be unavailable");
    }
  } else {
    XELOGD(
        "Failed to load D3DCompiler_47.dll, DXBC disassembly for debugging "
        "will be unavailable");
  }

  // Load optional dxilconv.dll.
  pfn_dxilconv_dxc_create_instance_ = nullptr;
  library_dxilconv_ = LoadLibraryW(L"dxilconv.dll");
  if (library_dxilconv_) {
    pfn_dxilconv_dxc_create_instance_ = DxcCreateInstanceProc(
        GetProcAddress(library_dxilconv_, "DxcCreateInstance"));
    if (pfn_dxilconv_dxc_create_instance_ == nullptr) {
      XELOGD(
          "Failed to get DxcCreateInstance from dxilconv.dll, converted DXIL "
          "disassembly for debugging will be unavailable");
    }
  } else {
    XELOGD(
        "Failed to load dxilconv.dll, converted DXIL disassembly for debugging "
        "will be unavailable - DXIL may be unsupported by your OS version");
  }

  // Load optional dxcompiler.dll.
  pfn_dxcompiler_dxc_create_instance_ = nullptr;
  library_dxcompiler_ = LoadLibraryW(L"dxcompiler.dll");
  if (library_dxcompiler_) {
    pfn_dxcompiler_dxc_create_instance_ = DxcCreateInstanceProc(
        GetProcAddress(library_dxcompiler_, "DxcCreateInstance"));
    if (pfn_dxcompiler_dxc_create_instance_ == nullptr) {
      XELOGD(
          "Failed to get DxcCreateInstance from dxcompiler.dll, converted DXIL "
          "disassembly for debugging will be unavailable");
    }
  } else {
    XELOGD(
        "Failed to load dxcompiler.dll, converted DXIL disassembly for "
        "debugging will be unavailable - if needed, download the DirectX "
        "Shader Compiler from "
        "https://github.com/microsoft/DirectXShaderCompiler/releases and place "
        "the DLL in the Xenia directory");
  }

  // Configure the DXGI debug info queue.
  if (cvars::d3d12_break_on_error || cvars::d3d12_break_on_warning) {
    IDXGIInfoQueue* dxgi_info_queue;
    if (SUCCEEDED(pfn_dxgi_get_debug_interface1_(
            0, IID_PPV_ARGS(&dxgi_info_queue)))) {
      if (cvars::d3d12_break_on_error) {
        dxgi_info_queue->SetBreakOnSeverity(
            DXGI_DEBUG_ALL, DXGI_INFO_QUEUE_MESSAGE_SEVERITY_CORRUPTION, true);
        dxgi_info_queue->SetBreakOnSeverity(
            DXGI_DEBUG_ALL, DXGI_INFO_QUEUE_MESSAGE_SEVERITY_ERROR, true);
      }
      if (cvars::d3d12_break_on_warning) {
        dxgi_info_queue->SetBreakOnSeverity(
            DXGI_DEBUG_ALL, DXGI_INFO_QUEUE_MESSAGE_SEVERITY_WARNING, true);
      }
      dxgi_info_queue->Release();
    }
  }

  // Enable the debug layer.
  bool debug = cvars::d3d12_debug;
  if (debug) {
    ID3D12Debug* debug_interface;
    if (SUCCEEDED(
            pfn_d3d12_get_debug_interface_(IID_PPV_ARGS(&debug_interface)))) {
      debug_interface->EnableDebugLayer();
      debug_interface->Release();
    } else {
      XELOGW("Failed to enable the Direct3D 12 debug layer");
      debug = false;
    }
  }

  // Device Removed Extended Data has to be set up before the device is
  // created. Not every system provides it.
  if (cvars::d3d12_dred) {
    ID3D12DeviceRemovedExtendedDataSettings* dred_settings;
    if (SUCCEEDED(
            pfn_d3d12_get_debug_interface_(IID_PPV_ARGS(&dred_settings)))) {
      dred_settings->SetAutoBreadcrumbsEnablement(
          D3D12_DRED_ENABLEMENT_FORCED_ON);
      dred_settings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
      dred_settings->Release();
      XELOGD3D("Direct3D 12 DRED enabled");
    } else {
      XELOGD3D("Direct3D 12 DRED is unavailable");
    }
  }

  // Create the DXGI factory.
  IDXGIFactory2* dxgi_factory;
  if (FAILED(pfn_create_dxgi_factory2_(debug ? DXGI_CREATE_FACTORY_DEBUG : 0,
                                       IID_PPV_ARGS(&dxgi_factory)))) {
    XELOGE("Failed to create a DXGI factory");
    return false;
  }

  // Choose the adapter.
  uint32_t adapter_index = 0;
  IDXGIAdapter1* adapter = nullptr;
  while (dxgi_factory->EnumAdapters1(adapter_index, &adapter) == S_OK) {
    DXGI_ADAPTER_DESC1 adapter_desc;
    if (SUCCEEDED(adapter->GetDesc1(&adapter_desc))) {
      if (SUCCEEDED(pfn_d3d12_create_device_(adapter, D3D_FEATURE_LEVEL_11_0,
                                             _uuidof(ID3D12Device), nullptr))) {
        if (cvars::d3d12_adapter >= 0) {
          if (adapter_index == cvars::d3d12_adapter) {
            break;
          }
        } else if (cvars::d3d12_adapter == -2) {
          if (adapter_desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
            break;
          }
        } else {
          if (!(adapter_desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
            break;
          }
        }
      }
    }
    adapter->Release();
    adapter = nullptr;
    ++adapter_index;
  }
  if (adapter == nullptr) {
    XELOGE(
        "Failed to get an adapter supporting Direct3D 12 with the feature "
        "level of at least 11_0");
    dxgi_factory->Release();
    return false;
  }
  DXGI_ADAPTER_DESC adapter_desc;
  if (FAILED(adapter->GetDesc(&adapter_desc))) {
    XELOGE("Failed to get the DXGI adapter description");
    adapter->Release();
    dxgi_factory->Release();
    return false;
  }
  adapter_vendor_id_ = GpuVendorID(adapter_desc.VendorId);
  adapter_device_id_ = adapter_desc.DeviceId;

  int adapter_name_mb_size = WideCharToMultiByte(
      CP_UTF8, 0, adapter_desc.Description, -1, nullptr, 0, nullptr, nullptr);
  if (adapter_name_mb_size != 0) {
    char* adapter_name_mb =
        reinterpret_cast<char*>(alloca(adapter_name_mb_size));
    if (WideCharToMultiByte(CP_UTF8, 0, adapter_desc.Description, -1,
                            adapter_name_mb, adapter_name_mb_size, nullptr,
                            nullptr) != 0) {
      adapter_description_ = adapter_name_mb;
      XELOGD3D("DXGI adapter: {} (vendor 0x{:04X}, device 0x{:04X})",
               adapter_name_mb, adapter_desc.VendorId, adapter_desc.DeviceId);
    }
  }

  // Create the Direct3D 12 device.
  ID3D12Device* device;
  if (FAILED(pfn_d3d12_create_device_(adapter, D3D_FEATURE_LEVEL_11_0,
                                      IID_PPV_ARGS(&device)))) {
    XELOGE("Failed to create a Direct3D 12 feature level 11_0 device");
    adapter->Release();
    dxgi_factory->Release();
    return false;
  }
  adapter->Release();

  // Configure the Direct3D 12 debug info queue.
  ID3D12InfoQueue* d3d12_info_queue;
  if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&d3d12_info_queue)))) {
    D3D12_MESSAGE_SEVERITY d3d12_info_queue_denied_severities[] = {
        D3D12_MESSAGE_SEVERITY_INFO,
    };
    D3D12_MESSAGE_ID d3d12_info_queue_denied_messages[] = {
        // Xbox 360 vertex fetch is explicit in shaders.
        D3D12_MESSAGE_ID_CREATEINPUTLAYOUT_EMPTY_LAYOUT,
        // Bug in the debug layer (fixed in some version of Windows) - gaps in
        // render target bindings must be represented with a fully typed RTV
        // descriptor and DXGI_FORMAT_UNKNOWN in the pipeline state, but older
        // debug layer versions give a format mismatch error in this case.
        D3D12_MESSAGE_ID_RENDER_TARGET_FORMAT_MISMATCH_PIPELINE_STATE,
        // Render targets and shader exports don't have to match on the Xbox
        // 360.
        D3D12_MESSAGE_ID_CREATEGRAPHICSPIPELINESTATE_RENDERTARGETVIEW_NOT_SET,
        // Arbitrary scissor can be specified by the guest, also it can be
        // explicitly used to disable drawing.
        D3D12_MESSAGE_ID_DRAW_EMPTY_SCISSOR_RECTANGLE,
        // Arbitrary clear values can be specified by the guest.
        D3D12_MESSAGE_ID_CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE,
        D3D12_MESSAGE_ID_CLEARDEPTHSTENCILVIEW_MISMATCHINGCLEARVALUE,
    };
    D3D12_INFO_QUEUE_FILTER d3d12_info_queue_filter = {};
    d3d12_info_queue_filter.DenyList.NumSeverities =
        UINT(xe::countof(d3d12_info_queue_denied_severities));
    d3d12_info_queue_filter.DenyList.pSeverityList =
        d3d12_info_queue_denied_severities;
    d3d12_info_queue_filter.DenyList.NumIDs =
        UINT(xe::countof(d3d12_info_queue_denied_messages));
    d3d12_info_queue_filter.DenyList.pIDList = d3d12_info_queue_denied_messages;
    d3d12_info_queue->PushStorageFilter(&d3d12_info_queue_filter);
    if (cvars::d3d12_break_on_error) {
      d3d12_info_queue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION,
                                           true);
      d3d12_info_queue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, true);
    }
    if (cvars::d3d12_break_on_warning) {
      d3d12_info_queue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING,
                                           true);
    }
    d3d12_info_queue->Release();
  }

  // Create the command queue for graphics.
  D3D12_COMMAND_QUEUE_DESC queue_desc;
  queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  if (cvars::d3d12_queue_priority >= 2) {
    queue_desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_GLOBAL_REALTIME;
    if (!EnableIncreaseBasePriorityPrivilege()) {
      XELOGW(
          "Failed to enable SeIncreaseBasePriorityPrivilege for global "
          "realtime Direct3D 12 command queue priority, falling back to high "
          "priority, try launching Xenia as administrator");
      queue_desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
    }
  } else if (cvars::d3d12_queue_priority >= 1) {
    queue_desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
  } else {
    queue_desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
  }
  queue_desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
  queue_desc.NodeMask = 0;
  ID3D12CommandQueue* direct_queue;
  if (FAILED(device->CreateCommandQueue(&queue_desc,
                                        IID_PPV_ARGS(&direct_queue)))) {
    bool queue_created = false;
    if (queue_desc.Priority == D3D12_COMMAND_QUEUE_PRIORITY_GLOBAL_REALTIME) {
      XELOGW(
          "Failed to create a Direct3D 12 direct command queue with global "
          "realtime priority, falling back to high priority, try launching "
          "Xenia as administrator");
      queue_desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
      queue_created = SUCCEEDED(
          device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&direct_queue)));
    }
    if (!queue_created) {
      XELOGE("Failed to create a Direct3D 12 direct command queue");
      device->Release();
      dxgi_factory->Release();
      return false;
    }
  }

  // Named for Device Removed Extended Data reports.
  direct_queue->SetName(L"Xenia direct queue");

  dxgi_factory_ = dxgi_factory;
  device_ = device;
  direct_queue_ = direct_queue;

  // Get descriptor sizes for each type.
  for (uint32_t i = 0; i < D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES; ++i) {
    descriptor_sizes_[i] =
        device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE(i));
  }

  // Check if optional features are supported.
  // D3D12_HEAP_FLAG_CREATE_NOT_ZEROED requires Windows 10 2004 (indicated by
  // the availability of ID3D12Device8 or D3D12_FEATURE_D3D12_OPTIONS7).
  heap_flag_create_not_zeroed_ = D3D12_HEAP_FLAG_NONE;
  D3D12_FEATURE_DATA_D3D12_OPTIONS7 options7;
  if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7,
                                            &options7, sizeof(options7)))) {
    heap_flag_create_not_zeroed_ = D3D12_HEAP_FLAG_CREATE_NOT_ZEROED;
  }
  ps_specified_stencil_reference_supported_ = false;
  rasterizer_ordered_views_supported_ = false;
  resource_binding_tier_ = D3D12_RESOURCE_BINDING_TIER_1;
  tiled_resources_tier_ = D3D12_TILED_RESOURCES_TIER_NOT_SUPPORTED;
  unaligned_block_textures_supported_ = false;
  D3D12_FEATURE_DATA_D3D12_OPTIONS options;
  if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS,
                                            &options, sizeof(options)))) {
    ps_specified_stencil_reference_supported_ =
        bool(options.PSSpecifiedStencilRefSupported);
    rasterizer_ordered_views_supported_ = bool(options.ROVsSupported);
    resource_binding_tier_ = options.ResourceBindingTier;
    tiled_resources_tier_ = options.TiledResourcesTier;
  }
  programmable_sample_positions_tier_ =
      D3D12_PROGRAMMABLE_SAMPLE_POSITIONS_TIER_NOT_SUPPORTED;
  D3D12_FEATURE_DATA_D3D12_OPTIONS2 options2;
  if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS2,
                                            &options2, sizeof(options2)))) {
    programmable_sample_positions_tier_ =
        options2.ProgrammableSamplePositionsTier;
  }
  D3D12_FEATURE_DATA_D3D12_OPTIONS8 options8;
  if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS8,
                                            &options8, sizeof(options8)))) {
    unaligned_block_textures_supported_ =
        bool(options8.UnalignedBlockTexturesSupported);
  }
  alpha_blend_factor_supported_ = false;
  D3D12_FEATURE_DATA_D3D12_OPTIONS13 options13 = {};
  if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS13,
                                            &options13, sizeof(options13)))) {
    alpha_blend_factor_supported_ = bool(options13.AlphaBlendFactorSupported);
  }
  virtual_address_bits_per_resource_ = 0;
  D3D12_FEATURE_DATA_GPU_VIRTUAL_ADDRESS_SUPPORT virtual_address_support;
  if (SUCCEEDED(device->CheckFeatureSupport(
          D3D12_FEATURE_GPU_VIRTUAL_ADDRESS_SUPPORT, &virtual_address_support,
          sizeof(virtual_address_support)))) {
    virtual_address_bits_per_resource_ =
        virtual_address_support.MaxGPUVirtualAddressBitsPerResource;
  }
  XELOGD3D(
      "Direct3D 12 device and OS features:\n"
      "* Max GPU virtual address bits per resource: {}\n"
      "* Non-zeroed heap creation: {}\n"
      "* Pixel-shader-specified stencil reference: {}\n"
      "* Programmable sample positions: tier {}\n"
      "* Rasterizer-ordered views: {}\n"
      "* Scalar alpha blend factor: {}\n"
      "* Resource binding: tier {}\n"
      "* Tiled resources: tier {}\n"
      "* Unaligned block-compressed textures: {}",
      virtual_address_bits_per_resource_,
      (heap_flag_create_not_zeroed_ & D3D12_HEAP_FLAG_CREATE_NOT_ZEROED) ? "yes"
                                                                         : "no",
      ps_specified_stencil_reference_supported_ ? "yes" : "no",
      uint32_t(programmable_sample_positions_tier_),
      rasterizer_ordered_views_supported_ ? "yes" : "no",
      alpha_blend_factor_supported_ ? "yes" : "no",
      uint32_t(resource_binding_tier_), uint32_t(tiled_resources_tier_),
      unaligned_block_textures_supported_ ? "yes" : "no");

  LogVideoMemoryInfo("after device creation");
  {
    IDXGIFactory4* dxgi_factory_4;
    if (SUCCEEDED(dxgi_factory_->QueryInterface(IID_PPV_ARGS(&dxgi_factory_4)))) {
      if (FAILED(dxgi_factory_4->EnumAdapterByLuid(
              device_->GetAdapterLuid(), IID_PPV_ARGS(&memory_info_adapter_)))) {
        memory_info_adapter_ = nullptr;
      }
      dxgi_factory_4->Release();
    }
    if (memory_info_adapter_) {
      xe::crash_report::AddStatusSource(DescribeMemoryStatus, this);
    }
  }

  // Get the graphics analysis interface, will silently fail if PIX is not
  // attached.
#if !XE_PLATFORM_WINRT
  pfn_dxgi_get_debug_interface1_(0, IID_PPV_ARGS(&graphics_analysis_));
#endif
  return true;
}
uint32_t D3D12Provider::CreateUploadResource(
    D3D12_HEAP_FLAGS HeapFlags, _In_ const D3D12_RESOURCE_DESC* pDesc,
    D3D12_RESOURCE_STATES InitialResourceState, REFIID riidResource,
    void** ppvResource, const D3D12_CLEAR_VALUE* pOptimizedClearValue) const {
  auto device = GetDevice();

  if (FAILED(device->CreateCommittedResource(
          &ui::d3d12::util::kHeapPropertiesUpload, HeapFlags, pDesc,
          InitialResourceState, pOptimizedClearValue, riidResource,
          ppvResource))) {
    XELOGE("Failed to create the gamma ramp upload buffer");
    return UPLOAD_RESULT_CREATE_FAILED;
  }

  return UPLOAD_RESULT_CREATE_SUCCESS;
}
std::unique_ptr<Presenter> D3D12Provider::CreatePresenter(
    Presenter::HostGpuLossCallback host_gpu_loss_callback) {
  return D3D12Presenter::Create(host_gpu_loss_callback, *this);
}

std::unique_ptr<ImmediateDrawer> D3D12Provider::CreateImmediateDrawer() {
  return D3D12ImmediateDrawer::Create(*this);
}

void D3D12Provider::LogD3D12DebugMessages() const {
  if (!device_) {
    return;
  }

  ID3D12InfoQueue* info_queue = nullptr;
  if (FAILED(device_->QueryInterface(IID_PPV_ARGS(&info_queue)))) {
    return;
  }

  UINT64 message_count = info_queue->GetNumStoredMessages();
  for (UINT64 i = 0; i < message_count; ++i) {
    SIZE_T message_size = 0;
    if (FAILED(info_queue->GetMessage(i, nullptr, &message_size))) {
      continue;
    }

    D3D12_MESSAGE* message =
        reinterpret_cast<D3D12_MESSAGE*>(alloca(message_size));
    if (FAILED(info_queue->GetMessage(i, message, &message_size))) {
      continue;
    }

    const char* severity_str = "INFO";
    switch (message->Severity) {
      case D3D12_MESSAGE_SEVERITY_CORRUPTION:
        severity_str = "CORRUPTION";
        break;
      case D3D12_MESSAGE_SEVERITY_ERROR:
        severity_str = "ERROR";
        break;
      case D3D12_MESSAGE_SEVERITY_WARNING:
        severity_str = "WARNING";
        break;
      case D3D12_MESSAGE_SEVERITY_INFO:
        severity_str = "INFO";
        break;
      case D3D12_MESSAGE_SEVERITY_MESSAGE:
        severity_str = "MESSAGE";
        break;
    }

    if (message->Severity == D3D12_MESSAGE_SEVERITY_ERROR ||
        message->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION) {
      XELOGE("D3D12 {}: [ID {}] {}", severity_str,
             static_cast<int>(message->ID), message->pDescription);
    } else if (message->Severity == D3D12_MESSAGE_SEVERITY_WARNING) {
      XELOGW("D3D12 {}: [ID {}] {}", severity_str,
             static_cast<int>(message->ID), message->pDescription);
    } else {
      XELOGI("D3D12 {}: [ID {}] {}", severity_str,
             static_cast<int>(message->ID), message->pDescription);
    }
  }

  info_queue->ClearStoredMessages();
  info_queue->Release();
}

namespace {

const char* DeviceRemovedReasonDescription(HRESULT reason) {
  switch (reason) {
    case S_OK:
      return "the device is not removed";
    case DXGI_ERROR_DEVICE_HUNG:
      return "DXGI_ERROR_DEVICE_HUNG, the GPU stopped responding to a command";
    case DXGI_ERROR_DEVICE_REMOVED:
      return "DXGI_ERROR_DEVICE_REMOVED, the GPU or its driver went away";
    case DXGI_ERROR_DEVICE_RESET:
      return "DXGI_ERROR_DEVICE_RESET, a badly formed command reset the GPU";
    case DXGI_ERROR_DRIVER_INTERNAL_ERROR:
      return "DXGI_ERROR_DRIVER_INTERNAL_ERROR";
    case DXGI_ERROR_INVALID_CALL:
      return "DXGI_ERROR_INVALID_CALL, the API was used in an invalid way";
    case E_OUTOFMEMORY:
      return "E_OUTOFMEMORY";
    default:
      return "unrecognized";
  }
}

const char* BreadcrumbOpName(D3D12_AUTO_BREADCRUMB_OP op) {
  switch (op) {
#define XE_D3D12_BREADCRUMB_OP(name) \
  case D3D12_AUTO_BREADCRUMB_OP_##name:  \
    return #name;
    XE_D3D12_BREADCRUMB_OP(SETMARKER)
    XE_D3D12_BREADCRUMB_OP(BEGINEVENT)
    XE_D3D12_BREADCRUMB_OP(ENDEVENT)
    XE_D3D12_BREADCRUMB_OP(DRAWINSTANCED)
    XE_D3D12_BREADCRUMB_OP(DRAWINDEXEDINSTANCED)
    XE_D3D12_BREADCRUMB_OP(EXECUTEINDIRECT)
    XE_D3D12_BREADCRUMB_OP(DISPATCH)
    XE_D3D12_BREADCRUMB_OP(COPYBUFFERREGION)
    XE_D3D12_BREADCRUMB_OP(COPYTEXTUREREGION)
    XE_D3D12_BREADCRUMB_OP(COPYRESOURCE)
    XE_D3D12_BREADCRUMB_OP(COPYTILES)
    XE_D3D12_BREADCRUMB_OP(RESOLVESUBRESOURCE)
    XE_D3D12_BREADCRUMB_OP(CLEARRENDERTARGETVIEW)
    XE_D3D12_BREADCRUMB_OP(CLEARUNORDEREDACCESSVIEW)
    XE_D3D12_BREADCRUMB_OP(CLEARDEPTHSTENCILVIEW)
    XE_D3D12_BREADCRUMB_OP(RESOURCEBARRIER)
    XE_D3D12_BREADCRUMB_OP(EXECUTEBUNDLE)
    XE_D3D12_BREADCRUMB_OP(PRESENT)
    XE_D3D12_BREADCRUMB_OP(RESOLVEQUERYDATA)
    XE_D3D12_BREADCRUMB_OP(BEGINSUBMISSION)
    XE_D3D12_BREADCRUMB_OP(ENDSUBMISSION)
    XE_D3D12_BREADCRUMB_OP(ATOMICCOPYBUFFERUINT)
    XE_D3D12_BREADCRUMB_OP(ATOMICCOPYBUFFERUINT64)
    XE_D3D12_BREADCRUMB_OP(RESOLVESUBRESOURCEREGION)
    XE_D3D12_BREADCRUMB_OP(WRITEBUFFERIMMEDIATE)
    XE_D3D12_BREADCRUMB_OP(SETPIPELINESTATE1)
    XE_D3D12_BREADCRUMB_OP(BARRIER)
    XE_D3D12_BREADCRUMB_OP(BEGIN_COMMAND_LIST)
#undef XE_D3D12_BREADCRUMB_OP
    default:
      return nullptr;
  }
}

const char* DredAllocationTypeName(D3D12_DRED_ALLOCATION_TYPE type) {
  switch (type) {
    case D3D12_DRED_ALLOCATION_TYPE_COMMAND_QUEUE:
      return "command queue";
    case D3D12_DRED_ALLOCATION_TYPE_COMMAND_ALLOCATOR:
      return "command allocator";
    case D3D12_DRED_ALLOCATION_TYPE_PIPELINE_STATE:
      return "pipeline state";
    case D3D12_DRED_ALLOCATION_TYPE_COMMAND_LIST:
      return "command list";
    case D3D12_DRED_ALLOCATION_TYPE_FENCE:
      return "fence";
    case D3D12_DRED_ALLOCATION_TYPE_DESCRIPTOR_HEAP:
      return "descriptor heap";
    case D3D12_DRED_ALLOCATION_TYPE_HEAP:
      return "heap";
    case D3D12_DRED_ALLOCATION_TYPE_QUERY_HEAP:
      return "query heap";
    case D3D12_DRED_ALLOCATION_TYPE_COMMAND_SIGNATURE:
      return "command signature";
    case D3D12_DRED_ALLOCATION_TYPE_RESOURCE:
      return "resource";
    default:
      return "object";
  }
}

std::string DebugObjectName(const char* name_a, const wchar_t* name_w) {
  if (name_w && *name_w) {
    return xe::to_utf8(
        std::u16string_view(reinterpret_cast<const char16_t*>(name_w)));
  }
  if (name_a && *name_a) {
    return name_a;
  }
  return "unnamed";
}

void LogDredAllocations(const char* label,
                        const D3D12_DRED_ALLOCATION_NODE* node) {
  for (uint32_t i = 0; node && i < 16; node = node->pNext, ++i) {
    XELOGE("DRED:   {} {} \"{}\"", label,
           DredAllocationTypeName(node->AllocationType),
           DebugObjectName(node->ObjectNameA, node->ObjectNameW));
  }
}

}  // namespace

void D3D12Provider::LogDeviceRemoval(const char* noticed_by) const {
  if (!device_ || device_removal_logged_.test_and_set()) {
    return;
  }
  const HRESULT reason = device_->GetDeviceRemovedReason();
  XELOGE("Direct3D 12 device lost, noticed by {}: 0x{:08X} ({})", noticed_by,
         uint32_t(reason), DeviceRemovedReasonDescription(reason));
  LogVideoMemoryInfo("at the device loss");

  ID3D12DeviceRemovedExtendedData* dred;
  if (FAILED(device_->QueryInterface(IID_PPV_ARGS(&dred)))) {
    XELOGE("DRED: no data (not supported here, or d3d12_dred is off)");
    xe::FlushLog();
    return;
  }

  D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT breadcrumbs = {};
  const HRESULT breadcrumbs_result =
      dred->GetAutoBreadcrumbsOutput(&breadcrumbs);
  if (FAILED(breadcrumbs_result)) {
    XELOGE("DRED: no breadcrumbs (0x{:08X})", uint32_t(breadcrumbs_result));
  } else {
    uint32_t incomplete_lists = 0;
    for (const D3D12_AUTO_BREADCRUMB_NODE* node =
             breadcrumbs.pHeadAutoBreadcrumbNode;
         node; node = node->pNext) {
      const uint32_t count = node->BreadcrumbCount;
      const uint32_t completed =
          node->pLastBreadcrumbValue ? *node->pLastBreadcrumbValue : 0;
      if (completed >= count) {
        continue;
      }
      if (++incomplete_lists > 8) {
        XELOGE("DRED: more command lists did not complete");
        break;
      }
      XELOGE(
          "DRED: command list \"{}\" on queue \"{}\" completed {} of {} "
          "operations",
          DebugObjectName(node->pCommandListDebugNameA,
                          node->pCommandListDebugNameW),
          DebugObjectName(node->pCommandQueueDebugNameA,
                          node->pCommandQueueDebugNameW),
          completed, count);
      if (!node->pCommandHistory) {
        continue;
      }
      const uint32_t first = completed > 8 ? completed - 8 : 0;
      const uint32_t end = std::min(count, completed + 4);
      for (uint32_t i = first; i < end; ++i) {
        const D3D12_AUTO_BREADCRUMB_OP op = node->pCommandHistory[i];
        const char* op_name = BreadcrumbOpName(op);
        XELOGE("DRED:   {} {} {}", i == completed ? "->" : "  ", i,
               op_name ? std::string(op_name)
                       : fmt::format("operation {}", int(op)));
      }
    }
    if (!incomplete_lists) {
      XELOGE("DRED: every recorded command list completed");
    }
  }

  D3D12_DRED_PAGE_FAULT_OUTPUT page_fault = {};
  const HRESULT page_fault_result =
      dred->GetPageFaultAllocationOutput(&page_fault);
  if (FAILED(page_fault_result)) {
    XELOGE("DRED: no page fault data (0x{:08X})", uint32_t(page_fault_result));
  } else if (!page_fault.PageFaultVA) {
    XELOGE("DRED: no GPU page fault");
  } else {
    XELOGE("DRED: GPU page fault at virtual address 0x{:016X}",
           uint64_t(page_fault.PageFaultVA));
    LogDredAllocations("allocated there:",
                       page_fault.pHeadExistingAllocationNode);
    LogDredAllocations("recently freed there:",
                       page_fault.pHeadRecentFreedAllocationNode);
  }
  dred->Release();
  xe::FlushLog();
}

void D3D12Provider::DescribeMemoryStatus(void* context,
                                         xe::crash_report::Writer& out) {
  auto provider = static_cast<const D3D12Provider*>(context);
  DXGI_QUERY_VIDEO_MEMORY_INFO local = {};
  if (provider->memory_info_adapter_ &&
      SUCCEEDED(provider->memory_info_adapter_->QueryVideoMemoryInfo(
          0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local))) {
    out.Append("GPU memory {} MiB of a {} MiB budget",
               uint64_t(local.CurrentUsage >> 20), uint64_t(local.Budget >> 20));
  } else {
    out.AppendText("GPU memory unknown");
  }
}

void D3D12Provider::LogVideoMemoryInfo(const char* when) const {
  if (!device_ || !dxgi_factory_) {
    return;
  }
  IDXGIFactory4* dxgi_factory_4;
  if (FAILED(dxgi_factory_->QueryInterface(IID_PPV_ARGS(&dxgi_factory_4)))) {
    return;
  }
  IDXGIAdapter3* adapter_3;
  const HRESULT adapter_result = dxgi_factory_4->EnumAdapterByLuid(
      device_->GetAdapterLuid(), IID_PPV_ARGS(&adapter_3));
  dxgi_factory_4->Release();
  if (FAILED(adapter_result)) {
    return;
  }
  DXGI_QUERY_VIDEO_MEMORY_INFO local = {};
  DXGI_QUERY_VIDEO_MEMORY_INFO non_local = {};
  const bool have_local = SUCCEEDED(adapter_3->QueryVideoMemoryInfo(
      0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local));
  const bool have_non_local = SUCCEEDED(adapter_3->QueryVideoMemoryInfo(
      0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &non_local));
  adapter_3->Release();
  if (have_local) {
    XELOGD3D("GPU memory {}: local {} MiB in use, budget {} MiB", when,
             local.CurrentUsage >> 20, local.Budget >> 20);
  }
  if (have_non_local) {
    XELOGD3D("GPU memory {}: non-local {} MiB in use, budget {} MiB", when,
             non_local.CurrentUsage >> 20, non_local.Budget >> 20);
  }
}

}  // namespace d3d12
}  // namespace ui
}  // namespace xe
