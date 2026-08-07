#include "StickProtocol.h"

// The protocol implementation is intentionally inline in the public header so
// host tools and MCU components share one byte-for-byte parser.  This
// translation unit provides a conventional source-file integration point for
// build systems that collect common/*.cpp automatically.
