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

#if defined(ARDUINO)
#include <Arduino.h>
#if defined(ARDUINO_ARCH_NRF52) || defined(ARDUINO_NRF52_ADAFRUIT)
#include <Adafruit_TinyUSB.h>
#endif
#endif

#include "microStore/File.h"
#include "microStore/FileSystem.h"
#include "microStore/FileStore.h"
#include "microStore/HeapStore.h"
#include "microStore/Codec.h"
#include "microStore/TypedStore.h"

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

void setup() {

#if defined(ARDUINO)
	Serial.begin(115200);
	while (!Serial) {
		if (millis() > 5000)
			break;
		delay(500);
	}
	Serial.println("Serial initialized");

#if defined(ESP32)
	Serial.print("Total SRAM: ");
	Serial.println(ESP.getHeapSize());
	Serial.print("Free SRAM: ");
	Serial.println(ESP.getFreeHeap());
#elif defined(ARDUINO_ARCH_NRF52) || defined(ARDUINO_NRF52_ADAFRUIT)
	Serial.print("Total SRAM: ");
	Serial.println(dbgHeapTotal());
	Serial.print("Free SRAM: ");
	Serial.println(dbgHeapFree());
#endif
#endif

#if 0
#if defined(USTORE_USE_POSIXFS)
	microStore::FileSystem filesystem{microStore::Adapters::PosixFileSystem()};
	filesystem.init();
    microStore::FileStore filestore;
#if defined(ARDUINO)
    filestore.init(filesystem, "/test_filestore");
#else
    filestore.init(filesystem, "test_filestore");
#endif
	printf("put: foo=bar\n");
	filestore.put("foo", "bar");
	std::string value;
	filestore.get("foo", value);
	printf("got: foo=%s\n", value.c_str());
#endif
#endif

#if defined(USTORE_USE_UNIVERSALFS)
	microStore::FileSystem filesystem{microStore::Adapters::UniversalFileSystem()};
	filesystem.init();
	microStore::FileStore filestore;
#if defined(ARDUINO)
    filestore.init(filesystem, "/test_typedstore");
#else
    filestore.init(filesystem, "test_typedstore");
#endif
	microStore::TypedStore<std::string, std::string, microStore::FileStore> store(filestore);
	printf("put: foo=bar\n");
	store.put("foo", "bar");
	std::string value;
	store.get("foo", value);
	printf("got: foo=%s\n", value.c_str());
#endif

}

void loop() {
}

#if defined(ARDUINO)
int _write(int file, char *ptr, int len) {
    return Serial.write(ptr, len);
}
#else
int main(void) {
	setup();
	//while (true) {
	//	loop();
	//}
	return 0;
}
#endif
