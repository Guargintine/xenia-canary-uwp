// Copyright 2026 Xenia Canary. Released under the BSD license.
#ifndef XENIA_KERNEL_XAM_XAM_NET_ASYNC_H_
#define XENIA_KERNEL_XAM_XAM_NET_ASYNC_H_

namespace xe::kernel {
class XLiveAPI;
namespace xam {
// Call after async work and guest threads have stopped.
void ResetNetworkTitleState(XLiveAPI* api);
}
}

#endif  // XENIA_KERNEL_XAM_XAM_NET_ASYNC_H_
