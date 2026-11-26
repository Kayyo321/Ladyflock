set(SOURCES
    Policy/Compiler.cpp
    Policy/NetworkGate.cpp
    Policy/Types.cpp
)

ladybird_lib(LibPrivacy privacy EXPLICIT_SYMBOL_EXPORT)
target_link_libraries(LibPrivacy PRIVATE LibIPC LibURL)
