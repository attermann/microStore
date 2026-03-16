#include "microStore/Store.h"
#include "microStore/FileSystem.h"

#if defined(MICROSTORE_USE_POSIXFS)
#include "microStore/impl/PosixFileSystemImpl.h"
#endif
#if defined(MICROSTORE_USE_STDIOFS)
#include "microStore/impl/StdioFileSystemImpl.h"
#endif
#if defined(MICROSTORE_USE_LITTLEFS)
#include "microStore/impl/LittleFSFileSystemImpl.h"
#endif
#if defined(MICROSTORE_USE_SPIFFS)
#include "microStore/impl/SPIFFSFileSystemImpl.h"
#endif
#if defined(MICROSTORE_USE_INTERNALFS)
#include "microStore/impl/InternalFSFileSystemImpl.h"
#endif
#if defined(MICROSTORE_USE_FLASHFS)
#include "microStore/impl/FlashFSFileSystemImpl.h"
#endif

int main(void) {
}

void setup() {
}

void loop() {
}
