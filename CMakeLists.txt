cmake_minimum_required(VERSION 3.16)

# PS Vita build (default when VITASDK is set):
#     cmake -S . -B build && cmake --build build          -> build/gwvita.vpk
# Desktop build (keyboard or gamepad):
#     cmake -S . -B build-pc -DDESKTOP=ON && cmake --build build-pc
option(DESKTOP "Build for the host PC instead of the PS Vita" OFF)
# The game's sound (app_data/, about 28 MB) can ride inside the VPK, so there
# is nothing to copy by hand; or leave it out and copy app_data/ to
# ux0:data/gwvita/app_data/ yourself.
option(EMBED_DATA "Pack app_data/ into the VPK" ON)

if(NOT DESKTOP)
  if(NOT DEFINED ENV{VITASDK})
    message(FATAL_ERROR "VITASDK is not set. Run: export VITASDK=/usr/local/vitasdk  (or pass -DDESKTOP=ON)")
  endif()
  if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  endif()
endif()

project(gwvita C)
set(CMAKE_C_STANDARD 99)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_executable(gwvita src/main.c src/game.c src/plat_sdl.c)
target_compile_options(gwvita PRIVATE -Wall)

if(DESKTOP)
  find_package(SDL2 REQUIRED)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(VORBISFILE REQUIRED IMPORTED_TARGET vorbisfile)
  target_include_directories(gwvita PRIVATE ${SDL2_INCLUDE_DIRS})
  target_link_libraries(gwvita PRIVATE ${SDL2_LIBRARIES} PkgConfig::VORBISFILE m)
else()
  include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)

  set(VITA_APP_NAME "Neon Grid")         # placeholder name on the home screen
  set(VITA_TITLEID  "NGRD00001")
  set(VITA_VERSION  "00.10")

  target_include_directories(gwvita PRIVATE "$ENV{VITASDK}/arm-vita-eabi/include/SDL2")
  # Static libraries: order matters (vorbisfile -> vorbis -> ogg).
  # Packages: vdpm install sdl2 libvorbis libogg
  target_link_libraries(gwvita
    SDL2 vorbisfile vorbis ogg m
    SceDisplay_stub SceCtrl_stub SceAudio_stub SceAudioIn_stub SceSysmodule_stub
    SceGxm_stub SceCommonDialog_stub SceAppMgr_stub SceAppUtil_stub SceTouch_stub
    SceHid_stub SceMotion_stub ScePower_stub SceIme_stub SceProcessmgr_stub
  )

  set(VPK_FILES
    FILE sce_sys/icon0.png sce_sys/icon0.png
    FILE sce_sys/livearea/contents/bg.png sce_sys/livearea/contents/bg.png
    FILE sce_sys/livearea/contents/startup.png sce_sys/livearea/contents/startup.png
    FILE sce_sys/livearea/contents/template.xml sce_sys/livearea/contents/template.xml
  )
  if(EMBED_DATA AND EXISTS "${CMAKE_SOURCE_DIR}/app_data/manifest.txt")
    list(APPEND VPK_FILES FILE app_data app_data)
  elseif(EMBED_DATA)
    message(WARNING "app_data/ not made yet (run tools/prep_assets.py): the VPK will be silent, "
                    "unless app_data is copied to ux0:data/gwvita/app_data/.")
  endif()

  vita_create_self(gwvita.self gwvita)
  vita_create_vpk(gwvita.vpk ${VITA_TITLEID} gwvita.self
    VERSION ${VITA_VERSION}
    NAME "${VITA_APP_NAME}"
    ${VPK_FILES}
  )
endif()
