project(obs-text)

if(WIN32)
	set(obs-text_PLATFORM_SOURCES
		gdiplus/obs-text.cpp)
endif()

set(obs-text_SOURCES
	)

add_library(obs-text MODULE
	${obs-text_PLATFORM_SOURCES}
	${obs-text_SOURCES})
target_link_libraries(obs-text
	libobs
	gdiplus)

install_obs_plugin_with_data(obs-text data)
