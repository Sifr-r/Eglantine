// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>

#include "common/logging.h"
#include "core/hle/kernel/k_auto_object_container.h"
#include "core/hle/kernel/kernel.h"

namespace Kernel {

void KAutoObjectWithListContainer::Register(KAutoObjectWithList* obj) {
    // KScopedInterruptDisable di;
    KScopedSpinLock lk(m_lock);

    if (m_finalized) {
        // The list was emptied already. Registering now would link an object whose memory the
        // system may have freed already, and the container would walk it when it is destroyed.
        LOG_ERROR(Kernel, "Object of type {} registered after the object list was finalized",
                  obj->GetTypeName());
        return;
    }

    m_object_list.insert_unique(*obj);
}

void KAutoObjectWithListContainer::Unregister(KAutoObjectWithList* obj) {
    // KScopedInterruptDisable di;
    KScopedSpinLock lk(m_lock);

    if (m_finalized) {
        // The list was emptied already, so the object is not in it any more. Unlinking it now
        // would touch a tree that does not hold it.
        return;
    }

    if (obj->is_linked()) {
        m_object_list.erase(m_object_list.iterator_to(*obj));
    }
}

void KAutoObjectWithListContainer::Finalize() {
    // KScopedInterruptDisable di;
    KScopedSpinLock lk(m_lock);

    if (m_finalized) {
        return;
    }
    m_finalized = true;

    // Unlink every object that is still registered. The objects live in the guest RAM arena,
    // which the system frees before the kernel is destroyed, so the container must not walk
    // its list after that point. Unlinking also clears the link flag of every object, so an
    // object that is closed later does not try to erase itself from the emptied list.
    while (!m_object_list.empty()) {
        auto* obj = &*m_object_list.begin();
        m_object_list.erase(m_object_list.iterator_to(*obj));
    }
}

size_t KAutoObjectWithListContainer::GetOwnedCount(KProcess* owner) {
    // KScopedInterruptDisable di;
    KScopedSpinLock lk(m_lock);

    return std::count_if(m_object_list.begin(), m_object_list.end(),
                         [&](const auto& obj) { return obj.GetOwner() == owner; });
}

} // namespace Kernel
