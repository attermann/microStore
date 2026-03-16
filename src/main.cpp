#include "microStore/Store.hpp"
#include "microStore/FileSystem.hpp"

#if defined(MICROSTORE_USE_POSIXFS)
#include "microStore/impl/PosixFileSystemImpl.hpp"
#endif
#if defined(MICROSTORE_USE_LITTLEFS)
#include "microStore/impl/LittleFSFileSystemImpl.hpp"
#endif
#if defined(MICROSTORE_USE_SPIFFS)
#include "microStore/impl/SPIFFSFileSystemImpl.hpp"
#endif
#if defined(MICROSTORE_USE_INTERNALFS)
#include "microStore/impl/InternalFSFileSystemImpl.hpp"
#endif
#if defined(MICROSTORE_USE_FLASHFS)
#include "microStore/impl/FlashFSFileSystemImpl.hpp"
#endif

int main(void) {
}

void setup() {
}

void loop() {
}
