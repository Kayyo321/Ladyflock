set(CMAKE_AUTOMOC OFF)
set(CMAKE_AUTORCC OFF)
set(CMAKE_AUTOUIC OFF)

set(GENERATED_SOURCES
    ${CMAKE_BINARY_DIR}/Services/ExtensionVerifier/ExtensionVerifierClientEndpoint.h
    ${CMAKE_BINARY_DIR}/Services/ExtensionVerifier/ExtensionVerifierServerEndpoint.h
)

set(SOURCES
    ConnectionFromClient.cpp
    XpiVerifier.cpp
)

add_library(extensionverifierservice STATIC ${SOURCES} ${GENERATED_SOURCES})

add_executable(ExtensionVerifier main.cpp)

if (LINUX)
    target_sources(ExtensionVerifier PRIVATE SandboxLinux.cpp)
elseif (APPLE)
    target_sources(ExtensionVerifier PRIVATE SandboxMacOS.cpp)
else()
    target_sources(ExtensionVerifier PRIVATE SandboxUnimplemented.cpp)
endif()

target_include_directories(extensionverifierservice PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/../..)
target_include_directories(extensionverifierservice PRIVATE ${LADYBIRD_SOURCE_DIR}/Services/)
target_include_directories(ExtensionVerifier PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/../..)
target_include_directories(ExtensionVerifier PRIVATE ${LADYBIRD_SOURCE_DIR}/Services/)

target_link_libraries(ExtensionVerifier PRIVATE extensionverifierservice LibCore LibIPC LibMain LibSandbox LibWebCommon OpenSSL::Crypto)
target_link_libraries(extensionverifierservice PUBLIC LibCore LibIPC LibMain LibWebCommon OpenSSL::Crypto)

if (WIN32)
    ladybird_windows_bin(ExtensionVerifier CONSOLE)
endif()

ladybird_sign_helper_process(ExtensionVerifier)
