// No-op tray backend for platforms without a StatusNotifierItem implementation
// here yet (currently everything except Linux). start() reports "no tray", so the
// app runs exactly as before. A macOS backend using NSStatusItem would replace
// this file with a TrayMac.mm, filling in the same Tray methods.

#include "platform/Tray.hpp"

namespace tt {

struct Tray::Impl {};

Tray::Tray() = default;
Tray::~Tray() { stop(); }

bool Tray::start() { return false; }
void Tray::stop() {}

} // namespace tt
