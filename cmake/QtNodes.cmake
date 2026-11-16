include(FetchContent)

set(VISIONPRO_QTNODES_ARCHIVE "" CACHE FILEPATH "Local copy of the pinned QtNodes source archive")

# QtNodes 3.0.16. Pin the commit and verify both remote and local archives.
set(qtnodes_archive "https://codeload.github.com/paceholder/nodeeditor/tar.gz/7c6341a66a8e46b8988140b9e60d892b6a3560b3")
if(VISIONPRO_QTNODES_ARCHIVE)
    set(qtnodes_archive "${VISIONPRO_QTNODES_ARCHIVE}")
endif()
FetchContent_Declare(qtnodes
    URL "${qtnodes_archive}"
    URL_HASH SHA256=e57e7092d6b0d115568eb852dcdbf09f975677821c0822e8976f112d046fe61f
    TLS_VERIFY TRUE
)

block()
    # The upstream minimum predates CMP0077. Keep its options local to QtNodes.
    set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
    set(BUILD_TESTING OFF)
    set(BUILD_EXAMPLES OFF)
    set(BUILD_DOCS OFF)
    set(BUILD_SHARED_LIBS OFF)
    set(USE_QT6 ON)
    set(QT_DIR "${Qt6_DIR}")
    FetchContent_MakeAvailable(qtnodes)
endblock()
