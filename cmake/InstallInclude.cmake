set(DESTINATION_INCLUDE_DIR "${CMAKE_INSTALL_INCLUDEDIR}/bcos-ledger")

install(
        DIRECTORY "ledger"
        DESTINATION "${DESTINATION_INCLUDE_DIR}"
        FILES_MATCHING PATTERN "*.h"
)

install(
        DIRECTORY "eventfilter"
        DESTINATION "${DESTINATION_INCLUDE_DIR}"
        FILES_MATCHING PATTERN "*.h"
)