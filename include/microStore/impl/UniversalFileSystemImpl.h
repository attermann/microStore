#pragma once

#if defined(USTORE_USE_UNIVERSALFS)

#include "../File.h"
#include "../FileSystem.h"

#if defined(ARDUINO_ARCH_NRF52) || defined(ARDUINO_NRF52_ADAFRUIT)

#define USTORE_USE_INTERNALFS
#include "InternalFSFileSystemImpl.h"
namespace microStoreImpl {
class UniversalFileSystemImpl : public microStoreImpl::InternalFSFileSystemImpl {};
}

#else

#define USTORE_USE_POSIXFS
#include "PosixFileSystemImpl.h"
namespace microStoreImpl {
class UniversalFileSystemImpl : public microStoreImpl::PosixFileSystemImpl {};
}

#endif

#endif
