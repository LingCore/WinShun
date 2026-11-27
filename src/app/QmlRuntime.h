#pragma once

#include <QtEnvironmentVariables>

// What every process with a QML engine sets before it makes one.
namespace ws::qmlruntime {

// Since Qt 6.8 the JavaScript engine collects garbage in steps, between
// events (QV4::MemoryManager::onEventLoop). A run of 0.50.0 ended there, idle
// in the tray after the PC had slept and woken (GitHub #490: c0000005 in
// GCStateMachine::transition), as other programs on Qt 6.8 and later have.
// QV4_GC_TIMELIMIT=0 is Qt's switch for collecting all at once, as it did
// before; the heaps here are small (the windows' bindings), so a collection
// is short either way. A value set from outside stays.
inline void prepare()
{
    if (!qEnvironmentVariableIsSet("QV4_GC_TIMELIMIT"))
        qputenv("QV4_GC_TIMELIMIT", "0");
}

} // namespace ws::qmlruntime
