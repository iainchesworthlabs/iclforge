# ---------------------------------------------------------------------------
# iclforge-app-text-encoding-tests - Catch2 coverage for apps/shared/text-encoding
# (cmake/IclforgeTests.cmake): the table of code pages and its names, what the operating system's
# conversion makes of known bytes in each page, the bytes a page does not have, and the pages
# handed to libs/lyrics as the decoder it asks for.
# ---------------------------------------------------------------------------
iclforge_add_test_binary(iclforge-app-text-encoding-tests PROJECT app-text-encoding SOURCES
    test_code_pages.cpp)
target_link_libraries(iclforge-app-text-encoding-tests PRIVATE iclforge::app_text_encoding)
