#pragma once

#include "Catalog.h"
#include "WebSearchConfig.h"

namespace ws::webplugin {

// Capability and installed/enabled state, rather than an INI switch alone:
// a missing, broken, disabled or older sample cannot leave phantom web rows.
inline bool available(const plugin::Catalog& catalog)
{
    const auto* entry = catalog.find(kId.toString());
    return entry && entry->manifest && entry->manifest->webSearch && catalog.enabled(entry->id);
}

} // namespace ws::webplugin
