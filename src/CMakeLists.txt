if(NOT MSVC)
	set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra")
endif()

set(SOURCES
	classic.c
	dynamics.c
	events.c
	guitar_hero_3.c
	io.c
	ir.c
	nunchuk.c
	wiiuse.c
	wiiboard.c
	classic.h
	definitions.h
	definitions_os.h
	dynamics.h
	events.h
	guitar_hero_3.h
	motion_plus.h
	motion_plus.c
	io.h
	ir.h
	nunchuk.h
	os.h
	tatacon.c
	tatacon.h
	util.c
	wiiuse_internal.h
	wiiboard.h)
set(API
	wiiuse.h)

if(WIN32)
	list(APPEND SOURCES os_win.c)
	set(CMAKE_DEBUG_POSTFIX _debug)
elseif(APPLE)
	# wiimosc patch: IOKit HID backend instead of the IOBluetooth one in os_mac/ (see os_mac_hid.c)
	list(APPEND SOURCES os_mac_hid.c)
elseif(WITH_BT_EMBEDDED)
	list(APPEND SOURCES os_bt_embedded.c)
else()
	# wiimosc patch: L2CAP backend without libbluetooth instead of os_nix.c (see os_linux_l2cap.c)
	list(APPEND SOURCES os_linux_l2cap.c)
endif()

if(MSVC)
	list(APPEND API wiiuse_msvcstdint.h)
endif()

add_definitions(-DWIIUSE_COMPILE_LIB)

add_library(wiiuse ${SOURCES} ${API})

if(WIN32)
	target_link_libraries(wiiuse ws2_32 setupapi ${WINHID_LIBRARIES})
elseif(WITH_BT_EMBEDDED)
	if(CMAKE_SYSTEM_NAME MATCHES "NintendoWii")
		set(EXTRA_LIBS ogc)
	else()
		set(EXTRA_LIBS rt)
	endif()
	target_link_libraries(wiiuse m PkgConfig::BTE ${EXTRA_LIBS})
elseif(LINUX)
	target_link_libraries(wiiuse m rt)
elseif(APPLE)
	# wiimosc patch: the IOKit HID backend needs IOKit and CoreFoundation only
	target_link_libraries(wiiuse "-framework IOKit" "-framework CoreFoundation")
endif()

set_property(TARGET
	wiiuse
	PROPERTY
	PUBLIC_HEADER
	${API})

set_property(TARGET wiiuse APPEND PROPERTY
  COMPILE_DEFINITIONS $<$<CONFIG:Debug>:WITH_WIIUSE_DEBUG>
)

set_target_properties(wiiuse PROPERTIES
	SOVERSION ${PROJECT_VERSION_MAJOR}
	VERSION ${PROJECT_VERSION})

install(TARGETS wiiuse
	RUNTIME DESTINATION bin
	COMPONENT runtime
	ARCHIVE
	DESTINATION
	${CMAKE_INSTALL_LIBDIR}
	COMPONENT
	development
	LIBRARY
	DESTINATION
	${CMAKE_INSTALL_LIBDIR}
	COMPONENT
	runtime
	PUBLIC_HEADER
	DESTINATION
	include
	COMPONENT
	development)
