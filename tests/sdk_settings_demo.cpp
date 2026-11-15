#include "winshun.h"

#include <string>

namespace {

class DemoPluginBase : public winshun::Plugin {
public:
protected:
    winshun::Items query(const winshun::Query&) override
    {
        return {winshun::Item("sdk-settings", "SDK settings demo").withData(winshun::Json::Object {
            {"showSettingsCalls", m_showSettingsCalls},
            {"settingsChangedCalls", m_settingsChangedCalls},
            {"settings", settings()}})};
    }

    void initialized() override
    {
        updateSettings(winshun::Json::Object {
            {"text", "from SDK"},
            {"number", 12.5},
            {"enabled", true},
            {"choice", "fast"}});
    }

    void settingsChanged(const winshun::Json&) override { ++m_settingsChangedCalls; }

    int showSettingsCalls() const { return m_showSettingsCalls; }
    int settingsChangedCalls() const { return m_settingsChangedCalls; }

protected:
    int m_showSettingsCalls = 0;
    int m_settingsChangedCalls = 0;
};

class DemoPlugin : public DemoPluginBase {
protected:
    void showSettings() override
    {
        ++m_showSettingsCalls;
    }
};

} // namespace

int main(int argc, char** argv)
{
    if (argc >= 2 && std::string(argv[1]) == "--default")
        return DemoPluginBase().serve();
    return DemoPlugin().serve();
}
