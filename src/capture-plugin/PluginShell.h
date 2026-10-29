#pragma once

namespace ws::captureplugin {

class Protocol;

// Whom shell::open and shell::reveal ask (PluginShell.cpp): set once, before
// the capture is made.
void setShellProtocol(Protocol* protocol);

} // namespace ws::captureplugin
