INCLUDE(FindPackageHandleStandardArgs)

FIND_LIBRARY(EFENCE_LIBRARY
		NAMES efence
)
MARK_AS_ADVANCED(EFENCE_LIBRARY)

FIND_PACKAGE_HANDLE_STANDARD_ARGS(efence DEFAULT_MSG EFENCE_LIBRARY)

IF(EFENCE_FOUND)
	SET(EFENCE_LIBRARIES ${EFENCE_LIBRARY})
ENDIF(EFENCE_FOUND)
