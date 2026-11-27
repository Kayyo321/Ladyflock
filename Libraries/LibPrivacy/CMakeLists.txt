set(SOURCES
    Identity/NavigationHandoff.cpp
    Identity/Types.cpp
    Navigation/TrackingParameters.cpp
    Policy/Compiler.cpp
    Policy/ManagedPolicy.cpp
    Policy/NetworkGate.cpp
    Policy/Route.cpp
    Policy/Types.cpp
    Storage/PartitionKey.cpp
)

ladybird_lib(LibPrivacy privacy EXPLICIT_SYMBOL_EXPORT)
target_link_libraries(LibPrivacy PRIVATE LibCore LibCrypto LibIPC LibURL)
