#pragma once

#if defined(USTORE_USE_UNIVERSALFS)

#include "../File.h"
#include "../FileSystem.h"

#if defined(ARDUINO_ARCH_NRF52) || defined(ARDUINO_NRF52_ADAFRUIT)

#ifndef USTORE_USE_INTERNALFS
#define USTORE_USE_INTERNALFS
#endif
#include "InternalFSFileSystem.h"
namespace microStore { namespace Adapters {
using UniversalFileSystem = InternalFSFileSystem;
} }

#else

#ifndef USTORE_USE_POSIXFS
#define USTORE_USE_POSIXFS
#endif
#include "PosixFileSystem.h"
namespace microStore { namespace Adapters {
using UniversalFileSystem = PosixFileSystem;
} }

#endif

#endif
