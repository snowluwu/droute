#pragma once

#include "pch.h"

namespace droute {

    bool IsInitializingRuntime();
    bool EnsureRuntime();
    void StopRuntime();

}
