#pragma once

// A touch on the dock or its menu becomes mouse clicks: tap = left, 500 ms still
// hold = right. NixOs-dots docs/manual.md#holding-in-hyprdrive
namespace Hyprdrive::TouchBridge {
    void init();
    void shutdown();
}
