# Original rigid-body, dynamic-entity and ragdoll implementation. Kept in a
# separate archive while its renderer/platform closure is brought up.
add_library(KisakCOD-vita-source-physics STATIC EXCLUDE_FROM_ALL
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/DynEntity/DynEntity_client.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/DynEntity/DynEntity_coll.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/DynEntity/DynEntity_pieces.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/physics/phys_coll_boxbrush.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/physics/phys_coll_capsulebrush.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/physics/phys_coll_cylinderbrush.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/physics/phys_contacts.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/physics/phys_ode.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/physics/phys_world_collision.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/ragdoll/ragdoll.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/ragdoll/ragdoll_controller.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/ragdoll/ragdoll_quat.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/ragdoll/ragdoll_update.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/physics/ode/collision_kernel.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/physics/ode/collision_quadtreespace.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/physics/ode/collision_space.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/physics/ode/collision_std.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/physics/ode/collision_transform.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/physics/ode/collision_trimesh_box.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/physics/ode/collision_util.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/physics/ode/error.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/physics/ode/export-dif.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/physics/ode/fastdot.c
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/physics/ode/fastldlt.c
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/physics/ode/fastlsolve.c
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/physics/ode/fastltsolve.c
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/physics/ode/joint.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/physics/ode/mass.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/physics/ode/matrix.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/physics/ode/ode.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/physics/ode/odemath.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/physics/ode/quickstep.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/physics/ode/rotation.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/physics/ode/timer.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/physics/ode/util.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/DynEntity/DynEntity_load_obj.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/universal/pool_allocator.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/qcommon/cm_showcollision.cpp
 )
target_compile_definitions(KisakCOD-vita-source-physics PRIVATE
    KISAK_MP=1 KISAK_OPENAL=1 KISAK_VITA_SLICE=1)
target_compile_options(KisakCOD-vita-source-physics PRIVATE
    -std=c++20 -fno-rtti -fno-exceptions -Wall -Wextra -fpermissive
    -ffunction-sections -fdata-sections)
target_include_directories(KisakCOD-vita-source-physics PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../../../src
    ${CMAKE_CURRENT_LIST_DIR}/../../../deps)
target_compile_options(KisakCOD-vita-source-physics PRIVATE
    -include universal/q_shared.h)
set_source_files_properties(
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/physics/ode/fastdot.c
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/physics/ode/fastldlt.c
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/physics/ode/fastlsolve.c
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/physics/ode/fastltsolve.c
    PROPERTIES LANGUAGE CXX)
