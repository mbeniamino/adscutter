#pragma once

#include "media/PacketIndex.h"

#include <functional>
#include <memory>

namespace adscutter {

// Builds a PacketIndex by reading every packet of the file once (no decoding).
//
// The format context must be freshly opened (positioned at the start).
// progress(fraction) is called periodically; returning false cancels the
// scan, in which case nullptr is returned with an empty error.
std::unique_ptr<PacketIndex> buildIndex(AVFormatContext *fmt,
                                        const std::function<bool(double)> &progress,
                                        QString *error);

} // namespace adscutter
