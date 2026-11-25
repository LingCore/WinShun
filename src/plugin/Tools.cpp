#include "Tools.h"

#include "SearchEngine.h"

namespace ws::plugin {

Tools::Tools(mcp::Services services, AppCatalog* apps, SystemCatalog* places, QObject* parent)
    : QObject(parent)
    , m_engine(std::make_unique<SearchEngine>(services.index, apps, places))
{
    services.engine = m_engine.get();
    m_host = std::make_unique<mcp::ToolHost>(std::move(services));
    setPolicy({});
}

Tools::~Tools() = default;

void Tools::setPolicy(mcp::Policy policy)
{
    policy.allowOpen = true; // the manifest's winshun:open gates it
    policy.allowClipboard = true; // ... and clipboard:read
    policy.allowDocuments = true; // ... and winshun:content (list_documents), winshun:search (list_files)
    policy.excludedPaths.clear(); // "不让 AI 看到的文件夹" is the AI's: a plugin runs as the user, who sees them
    m_host->setPolicy(std::move(policy));
}

} // namespace ws::plugin
