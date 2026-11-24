find_package(Qt6 6.8 REQUIRED COMPONENTS Widgets Multimedia OpenGLWidgets)
find_path(VEYRA_CUDA_INCLUDE_DIR cuda.h HINTS /opt/cuda/include /usr/local/cuda/include REQUIRED)
find_library(VEYRA_CUDA_DRIVER cuda HINTS /usr/lib /usr/lib/x86_64-linux-gnu REQUIRED)

add_executable(veyra-linux main.cpp Vfg.cpp Preview.cpp)
set_target_properties(veyra-linux PROPERTIES AUTOMOC ON)
target_compile_features(veyra-linux PRIVATE cxx_std_20)
target_include_directories(veyra-linux PRIVATE "${VEYRA_CUDA_INCLUDE_DIR}")
target_link_libraries(veyra-linux PRIVATE Qt6::Widgets Qt6::Multimedia Qt6::OpenGLWidgets
  "${VEYRA_CUDA_DRIVER}" ${CMAKE_DL_LIBS})
target_compile_options(veyra-linux PRIVATE -Wall -Wextra -Wpedantic)
install(TARGETS veyra-linux RUNTIME DESTINATION bin)
