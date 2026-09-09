ADD_EXECUTABLE(mesh_occ_mpi occmesh_MPI_main.cpp 3DNgmesher.cpp scaling_profiler.cpp research_mesh.cpp task_mesh.cpp)

TARGET_LINK_LIBRARIES(mesh_occ_mpi libnglib.so)

TARGET_LINK_LIBRARIES(mesh_occ_mpi ${OCC_LIBRARIES})

TARGET_LINK_LIBRARIES(mesh_occ_mpi TKernel)

TARGET_LINK_LIBRARIES(mesh_occ_mpi metis)

TARGET_LINK_LIBRARIES(mesh_occ_mpi  /vol8/home/hnu_lhz/cjz/NETGEN/install/lib/libGKlib.a)

# GCC 8 and older expose C++17 std::filesystem through a separate library.
IF(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9.0)
       TARGET_LINK_LIBRARIES(mesh_occ_mpi stdc++fs)
ENDIF()


INSTALL(TARGETS mesh_occ_mpi   
       RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
