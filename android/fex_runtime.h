/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
namespace Core::GuestCpu { class HleCallAdapter; }
void tempest_fex_add_range(uintptr_t address, size_t size, bool executable, bool writable);
uintptr_t tempest_fex_bind(const char *name, uintptr_t native_function);
uintptr_t tempest_fex_missing(const char *name);
void tempest_fex_signals();
void tempest_fex_register_adapter(uintptr_t native_function,
    std::shared_ptr<Core::GuestCpu::HleCallAdapter> adapter);
