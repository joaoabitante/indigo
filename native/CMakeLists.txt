cmake_minimum_required(VERSION 3.20)
file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/VERSION" INDIGO_VERSION_FILE LIMIT_COUNT 1)  # versão única do PC e do Android
project(IndigoNative VERSION ${INDIGO_VERSION_FILE} LANGUAGES CXX)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/VERSION")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  add_compile_options(-Wall -Wextra -Wno-unused-parameter)
  set(CMAKE_CXX_FLAGS_RELEASE "-O3")
endif()

# otimização no link (cerca de 13% mais rápido no GBA); desligue com -DINDIGO_LTO=OFF
option(INDIGO_LTO "Otimização no link (LTO)" ON)
if(INDIGO_LTO AND CMAKE_BUILD_TYPE STREQUAL "Release")
  include(CheckIPOSupported)
  check_ipo_supported(RESULT ipo_ok OUTPUT ipo_msg LANGUAGES CXX)
  if(ipo_ok)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
  endif()
endif()

# Linux (PC e Steam Deck): proteções do compilador e do link no aplicativo e nos núcleos (desligue com
# -DINDIGO_HARDENING=OFF). Windows e Android não mudam.
set(INDIGO_LINUX OFF)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT ANDROID)
  set(INDIGO_LINUX ON)
endif()
option(INDIGO_HARDENING "Linux: _FORTIFY_SOURCE, stack protector, PIE e RELRO completo" ON)
if(INDIGO_LINUX AND INDIGO_HARDENING AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  include(CheckPIESupported)
  check_pie_supported(LANGUAGES CXX)
  set(CMAKE_POSITION_INDEPENDENT_CODE ON)  # -fPIE na compilação e -pie no link dos executáveis
  add_compile_options(-fstack-protector-strong "$<$<NOT:$<CONFIG:Debug>>:-U_FORTIFY_SOURCE;-D_FORTIFY_SOURCE=2>")
  add_link_options(-Wl,-z,relro,-z,now -Wl,-z,noexecstack)
endif()

# núcleos (sem dependências externas)
add_library(indigo_core STATIC
  src/core/registry.cpp
  src/core/zip.cpp
  src/core/backup.cpp
  src/gba/cpu.cpp
  src/gba/bus.cpp
  src/gba/io.cpp
  src/gba/ppu.cpp
  src/gba/apu.cpp
  src/gba/hle.cpp
  src/gba/save.cpp
  src/gba/gba.cpp
  src/gb/cart.cpp
  src/gb/ppu.cpp
  src/gb/apu.cpp
  src/gb/gb.cpp
  src/gb/sgb.cpp
  src/nes/cpu.cpp
  src/nes/ppu.cpp
  src/nes/apu.cpp
  src/nes/mappers.cpp
  src/nes/mappers_ext.cpp
  src/nes/nes.cpp
  src/snes/cpu.cpp
  src/snes/spc700.cpp
  src/snes/apu.cpp
  src/snes/ppu.cpp
  src/snes/bus.cpp
  src/snes/gsu.cpp
  src/snes/dsp1.cpp
  src/snes/sa1.cpp
  src/snes/sdd1.cpp
  src/snes/spc7110.cpp
  src/sms/z80.cpp
  src/sms/vdp.cpp
  src/sms/psg.cpp
  src/sms/sms.cpp
  src/ngp/tlcs900h.cpp
  src/ngp/k2ge.cpp
  src/ngp/t6w28.cpp
  src/ngp/ngp.cpp
)
target_include_directories(indigo_core PUBLIC src)

# Etapa 7 (clássicos e portáteis menores): cada console numa pasta própria
file(GLOB INDIGO_ETAPA7_SRC CONFIGURE_DEPENDS
  src/a2600/*.cpp src/a7800/*.cpp src/atari/*.cpp src/a5200/*.cpp src/lynx/*.cpp src/ws/*.cpp src/vb/*.cpp src/pm/*.cpp src/tms/*.cpp src/msx/*.cpp src/vectrex/*.cpp
  src/md/*.cpp src/pce/*.cpp src/neogeo/*.cpp src/cps/*.cpp src/cdrom/*.cpp
  src/neogeocd/*.cpp src/segacd/*.cpp src/pcecd/*.cpp src/md32x/*.cpp
  src/ps1/*.cpp src/tdo/*.cpp src/fds/*.cpp src/svision/*.cpp src/gnw/*.cpp src/gamecom/*.cpp src/intv/*.cpp src/pcfx/*.cpp src/jaguar/*.cpp src/saturn/*.cpp)
target_sources(indigo_core PRIVATE ${INDIGO_ETAPA7_SRC})

# ferramentas de teste sem interface (desligadas no build do Android)
option(INDIGO_TOOLS "Compilar as ferramentas de teste" ON)
if(INDIGO_TOOLS)
add_executable(indigo-run tools/runrom.cpp)
target_link_libraries(indigo-run PRIVATE indigo_core)
add_executable(indigo-zextest tools/zextest.cpp)
target_link_libraries(indigo-zextest PRIVATE indigo_core)
add_executable(indigo-tlcsfuzz tools/tlcsfuzz.cpp)
target_link_libraries(indigo-tlcsfuzz PRIVATE indigo_core)
add_executable(indigo-gbafuzz tools/gbafuzz.cpp)
target_link_libraries(indigo-gbafuzz PRIVATE indigo_core)
add_executable(indigo-ziptest tools/ziptest.cpp)
target_link_libraries(indigo-ziptest PRIVATE indigo_core)
add_executable(indigo-backuptest tools/backuptest.cpp)
target_link_libraries(indigo-backuptest PRIVATE indigo_core)
add_executable(indigo-robustez tools/robustez.cpp)
add_executable(indigo-t_seguranca tools/t_seguranca.cpp)  # arquivos maliciosos montados no próprio teste
target_link_libraries(indigo-t_seguranca PRIVATE indigo_core)
add_executable(indigo-nes6502test tools/nes6502test.cpp)
add_executable(indigo-nestest tools/nestest.cpp)
add_executable(indigo-dsp1test tools/dsp1test.cpp)
add_executable(indigo-sa1test tools/sa1test.cpp)
add_executable(indigo-sdd1test tools/sdd1test.cpp)
add_executable(indigo-spc7110test tools/spc7110test.cpp)
add_executable(indigo-nesmappertest tools/nesmappertest.cpp)
add_executable(indigo-snes65816test tools/snes65816test.cpp)
add_executable(indigo-spc700test tools/spc700test.cpp)
add_executable(indigo-snestest tools/snestest.cpp)
target_link_libraries(indigo-robustez PRIVATE indigo_core)
# testes da etapa 7: tools/t7_<console>.cpp vira indigo-t7_<console>
file(GLOB INDIGO_T7_TOOLS CONFIGURE_DEPENDS tools/t7_*.cpp)
foreach(t ${INDIGO_T7_TOOLS})
  get_filename_component(tn ${t} NAME_WE)
  add_executable(indigo-${tn} ${t})
  target_link_libraries(indigo-${tn} PRIVATE indigo_core)
endforeach()
# testes da etapa 9 (PlayStation): tools/t9_<parte>.cpp vira indigo-t9_<parte> (GTE e SPU abrem a régua com dlopen)
file(GLOB INDIGO_T9_TOOLS CONFIGURE_DEPENDS tools/t9_*.cpp tools/t9b_*.cpp)
if(WIN32)  # o t9_spu abre a régua com dlopen (dlfcn.h não existe no MinGW): só fora do Windows
  list(FILTER INDIGO_T9_TOOLS EXCLUDE REGEX "/t9_spu\\.cpp$")
endif()
foreach(t ${INDIGO_T9_TOOLS})
  get_filename_component(tn ${t} NAME_WE)
  add_executable(indigo-${tn} ${t})
  target_link_libraries(indigo-${tn} PRIVATE indigo_core ${CMAKE_DL_LIBS})
endforeach()
target_link_libraries(indigo-nes6502test PRIVATE indigo_core)
target_link_libraries(indigo-nestest PRIVATE indigo_core)
target_link_libraries(indigo-dsp1test PRIVATE indigo_core)
target_link_libraries(indigo-sa1test PRIVATE indigo_core)
target_link_libraries(indigo-sdd1test PRIVATE indigo_core)
target_link_libraries(indigo-spc7110test PRIVATE indigo_core)
target_link_libraries(indigo-nesmappertest PRIVATE indigo_core)
target_link_libraries(indigo-snes65816test PRIVATE indigo_core)
target_link_libraries(indigo-spc700test PRIVATE indigo_core)
target_link_libraries(indigo-snestest PRIVATE indigo_core)
if(WIN32)
  target_link_options(indigo-run PRIVATE -static -static-libgcc -static-libstdc++)
  target_link_options(indigo-zextest PRIVATE -static -static-libgcc -static-libstdc++)
  target_link_options(indigo-tlcsfuzz PRIVATE -static -static-libgcc -static-libstdc++)
  target_link_options(indigo-robustez PRIVATE -static -static-libgcc -static-libstdc++)
  target_link_options(indigo-gbafuzz PRIVATE -static -static-libgcc -static-libstdc++)
endif()
endif()

# aplicativo com janela (SDL3)
option(INDIGO_APP "Compilar o aplicativo com janela (precisa do SDL3)" ON)
if(INDIGO_APP)
  find_package(SDL3 CONFIG REQUIRED)
  add_executable(indigo WIN32 src/app/main.cpp src/app/ui.cpp src/app/platform.cpp src/app/shelf.cpp src/app/cloud.cpp src/app/video.cpp src/app/text.cpp)
  include(cmake/embed.cmake)
  indigo_embed("${CMAKE_CURRENT_BINARY_DIR}/gen/fonts.h" indigo_font "${CMAKE_CURRENT_LIST_DIR}/res/fonts/Poppins-Regular.ttf" "${CMAKE_CURRENT_LIST_DIR}/res/fonts/Poppins-SemiBold.ttf")
  target_include_directories(indigo PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/gen")
  if(WIN32)
    target_link_libraries(indigo PRIVATE winhttp shell32)
  endif()
  if(INDIGO_LINUX)
    # Linux: SDL3 compartilhado, levado no pacote em lib/ ao lado do executável (tools/pacote-linux.sh);
    # o libstdc++ vai dentro do executável para rodar em distribuições com um gcc mais velho
    target_link_libraries(indigo PRIVATE indigo_core SDL3::SDL3)
    target_link_options(indigo PRIVATE -static-libstdc++ -static-libgcc)
    set_target_properties(indigo PROPERTIES INSTALL_RPATH "$ORIGIN/lib")
    # ícone da janela (no Windows ele vem do .exe)
    indigo_embed("${CMAKE_CURRENT_BINARY_DIR}/gen/icon.h" indigo_icon "${CMAKE_CURRENT_LIST_DIR}/res/indigo.png")
    target_compile_definitions(indigo PRIVATE INDIGO_WINDOW_ICON=1)
    install(TARGETS indigo RUNTIME DESTINATION .)
  else()
    target_link_libraries(indigo PRIVATE indigo_core SDL3::SDL3-static)
  endif()
  target_compile_definitions(indigo PRIVATE INDIGO_VERSION="${PROJECT_VERSION}")
  if(WIN32)
    # ícone e informações do programa
    enable_language(RC)
    set(INDIGO_RES_DIR "${CMAKE_CURRENT_LIST_DIR}/res")
    configure_file(res/indigo.rc.in "${CMAKE_CURRENT_BINARY_DIR}/indigo.rc" @ONLY)
    target_sources(indigo PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/indigo.rc")
    target_link_options(indigo PRIVATE -static -static-libgcc -static-libstdc++)
  endif()
endif()
