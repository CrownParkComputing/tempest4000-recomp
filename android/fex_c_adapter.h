/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include "fex_exports.h"
#include "core/guest_cpu/hle_call_adapter.h"

namespace Tempest {
class CAdapter final : public Core::GuestCpu::HleCallAdapter {
public:
    explicit CAdapter(const TempestFexExport &definition_) : definition(definition_) {}
    Core::GuestCpu::HleCallResult Invoke(Core::GuestCpu::HleCallFrame &frame) const override {
        if (definition.stack_bytes && (!frame.validate_range ||
            !frame.validate_range(frame.validate_context, frame.rsp + 8, definition.stack_bytes, false)))
            return Core::GuestCpu::HleCallFailure{EFAULT, Name()};
        TempestFexFrame call{frame.gpr.data(),
            reinterpret_cast<uint64_t (*)[2]>(frame.xmm.data()), frame.rsp};
        definition.invoke(&call);
        return true;
    }
private:
    TempestFexExport definition;
};
}
