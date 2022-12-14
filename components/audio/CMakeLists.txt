set(COMPONENT_ADD_INCLUDEDIRS ./include)

set(COMPONENT_REQUIRES )
set(COMPONENT_PRIV_REQUIRES )

set(COMPONENT_SRCS
        ./audio.c
        ./flac_wrapper.c
        ./audio_common.c
        ./mad_wrapper.c
        ./helix_wrapper.c
        ./wav_wrapper.c
        )

register_component()

if (DEFINED AAC_DISABLE_SBR)
    add_prebuilt_library(libhelix-aac 	lib/libhelix-aac.a )
else ()
    add_prebuilt_library(libhelix-aac 	lib/libhelix-aac-sbr.a )
endif()

add_prebuilt_library(libmad 		lib/libmad.a)
add_prebuilt_library(libFLAC 		lib/libFLAC.a )
add_prebuilt_library(libvorbisidec 	lib/libvorbisidec.a )
add_prebuilt_library(libogg 		lib/libogg.a )
add_prebuilt_library(libalac 		lib/libalac.a )
add_prebuilt_library(libresample16 	lib/libresample16.a )
add_prebuilt_library(libopusfile 	lib/libopusfile.a )
add_prebuilt_library(libopus 		lib/libopus.a )

target_link_libraries(${COMPONENT_LIB} INTERFACE libmad)
target_link_libraries(${COMPONENT_LIB} INTERFACE libFLAC)
target_link_libraries(${COMPONENT_LIB} INTERFACE libhelix-aac)
target_link_libraries(${COMPONENT_LIB} INTERFACE libvorbisidec)
target_link_libraries(${COMPONENT_LIB} INTERFACE libogg)
target_link_libraries(${COMPONENT_LIB} INTERFACE libalac)
target_link_libraries(${COMPONENT_LIB} INTERFACE libresample16)
target_link_libraries(${COMPONENT_LIB} INTERFACE libopusfile)
target_link_libraries(${COMPONENT_LIB} INTERFACE libopus)