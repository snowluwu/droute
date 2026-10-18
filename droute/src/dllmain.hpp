#pragma once

#include "pch.h"

namespace droute {

    void ReconnectWorker();
    bool IsInitializingRuntime();
    bool EnsureRuntime();
    void StopRuntime();

}
