#
# 这个文件中用于在导出时向上层依赖的项目传递下层依赖
#

# Boost
set(_boost_components program_options log json system)
find_package(Boost REQUIRED COMPONENTS ${_boost_components})
if(NOT Boost_USE_STATIC_LIBS)
  foreach(_boost_component IN LISTS _boost_components)
    target_link_libraries(Boost::${_boost_component}
                          INTERFACE Boost::dynamic_linking)
  endforeach()
endif()
