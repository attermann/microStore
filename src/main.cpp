#include "microStore/Store.h"
#include "microStore/File.h"
#include "microStore/FileSystem.h"
#include "microStore/Codec.h"
#include "microStore/Table.h"

#if defined(USTORE_USE_POSIXFS)
#include "microStore/impl/PosixFileSystemImpl.h"
#endif
#if defined(USTORE_USE_STDIOFS)
#include "microStore/impl/StdioFileSystemImpl.h"
#endif
#if defined(USTORE_USE_LITTLEFS)
#include "microStore/impl/LittleFSFileSystemImpl.h"
#endif
#if defined(USTORE_USE_SPIFFS)
#include "microStore/impl/SPIFFSFileSystemImpl.h"
#endif
#if defined(USTORE_USE_INTERNALFS)
#include "microStore/impl/InternalFSFileSystemImpl.h"
#endif
#if defined(USTORE_USE_FLASHFS)
#include "microStore/impl/FlashFSFileSystemImpl.h"
#endif
#if defined(USTORE_USE_UNIVERSALFS)
#include "microStore/impl/UniversalFileSystemImpl.h"
#endif
#include "microStore/impl/NoopFileSystemImpl.h"

int main(void) {

	//microStore::FileSystem filesystem(new microStoreImpl::PosixFileSystemImpl());
    //microStore::Store store;
    //store.init(filesystem, "ms_kvstore");

}

void setup() {
}

void loop() {
}
