#pragma once

namespace SimulatorLifecycle {

enum class WakeReason { None, PowerButton, Timer };

void initProcessArgs(char** argv);
WakeReason consumeWakeReason();
[[noreturn]] void rebootAsPowerWake();

}  // namespace SimulatorLifecycle
