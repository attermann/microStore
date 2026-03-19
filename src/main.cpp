/*
 * Copyright (c) 2026 Chad Attermann
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 */

#include "microStore/Store.h"
#include "microStore/File.h"
#include "microStore/FileSystem.h"
#include "microStore/Codec.h"
#include "microStore/Table.h"

#if defined(USTORE_USE_POSIXFS)
#include "microStore/Adapters/PosixFileSystem.h"
#endif
#if defined(USTORE_USE_STDIOFS)
#include "microStore/Adapters/StdioFileSystem.h"
#endif
#if defined(USTORE_USE_LITTLEFS)
#include "microStore/Adapters/LittleFSFileSystem.h"
#endif
#if defined(USTORE_USE_SPIFFS)
#include "microStore/Adapters/SPIFFSFileSystem.h"
#endif
#if defined(USTORE_USE_INTERNALFS)
#include "microStore/Adapters/InternalFSFileSystem.h"
#endif
#if defined(USTORE_USE_FLASHFS)
#include "microStore/Adapters/FlashFSFileSystem.h"
#endif
#if defined(USTORE_USE_UNIVERSALFS)
#include "microStore/Adapters/UniversalFileSystem.h"
#endif
#include "microStore/Adapters/NoopFileSystem.h"

int main(void) {

	//microStore::FileSystem filesystem{microStore::Adapters::PosixFileSystem()};
    //microStore::Store store;
    //store.init(filesystem, "ms_kvstore");

	microStore::FileSystem filesystem{microStore::Adapters::UniversalFileSystem()};

	return 0;
}

void setup() {
}

void loop() {
}
