ladybird_test_suite(LibPrivacy)

set(TEST_SOURCES
    TestPrivacyIdentityAndStorage.cpp
    TestPrivacyPolicy.cpp
    TestPrivacyRoute.cpp
)

target_sources(TestLibPrivacy PRIVATE ${TEST_SOURCES})
target_link_libraries(TestLibPrivacy PRIVATE LibIPC LibPrivacy LibURL)
