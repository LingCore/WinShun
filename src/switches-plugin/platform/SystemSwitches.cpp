#include "SystemSwitches.h"

#include "SwitchAudio.h"
#include "SwitchExplorer.h"
#include "SwitchKeepAwake.h"
#include "SwitchRadios.h"
#include "SwitchTheme.h"

namespace ws {

SwitchBackends systemSwitchBackends(const QString& dataDir)
{
    SwitchBackends backends;
    backends.push_back(std::make_unique<SwitchTheme>());
    backends.push_back(std::make_unique<SwitchExplorer>());
    backends.push_back(std::make_unique<SwitchRadios>());
    backends.push_back(std::make_unique<SwitchAudio>());
    backends.push_back(std::make_unique<SwitchKeepAwake>(
        [] { return QDateTime::currentDateTime(); },
        std::make_unique<LidOverride>(PowerPlans::system(), dataDir.isEmpty() ? QString() : dataDir + u"/lid-close.json")));
    return backends;
}

} // namespace ws
