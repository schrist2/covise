# PROJECT(clean_covise)

ADD_DEFINITIONS(
)

INCLUDE_DIRECTORIES(
)

SET(HEADERS
)

SET(SOURCES
  clean.c
)

IF(UNIX)
  FIND_LIBRARY(RT rt)
  IF(RT)
    SET(EXTRA_LIBS ${RT})
  ENDIF()
ENDIF()
ADD_COVISE_EXECUTABLE(clean_covise)

COVISE_WNOERROR(clean_covise BASEARCH rhel5)
TARGET_LINK_LIBRARIES(clean_covise ${EXTRA_LIBS})

COVISE_INSTALL_TARGET(clean_covise)
