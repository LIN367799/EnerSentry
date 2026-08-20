# ============================================================
# EnsDeploy.cmake —— 部署辅助函数
# ============================================================
# 当前仅实现 Windows 下 Qt 运行时的自动部署（windeployqt）。
# 后续若需打包 / 安装脚本，可在此扩展。
#
# 设计要点：
#   各 app 在自己的 CMakeLists 里调用 ens_windeployqt(<target>) 只是「登记」，
#   真正的部署在根 CMakeLists 末尾调用 ens_finalize_deploy() 时统一执行——
#   所有目标合并进【一个】串行 custom target，避免 Ninja 并行构建时多个
#   windeployqt 进程同时往同一个 bin/<CONFIG> 写同一批 Qt DLL（导致
#   "Cannot create ... for output" 的并发写冲突）。
# ============================================================

# 登记一个需要部署 Qt 运行时的可执行目标
function(ens_windeployqt target)
    if(NOT WIN32)
        return()
    endif()
    set_property(GLOBAL APPEND PROPERTY ENS_DEPLOY_TARGETS ${target})
endfunction()

# 在所有 add_subdirectory 之后调用：创建一个串行的部署目标
function(ens_finalize_deploy)
    get_property(_targets GLOBAL PROPERTY ENS_DEPLOY_TARGETS)
    if(NOT _targets)
        return()
    endif()

    # 从 Qt5 安装目录推导 windeployqt 路径
    # Qt5Core_DIR 形如 D:/HJL/qt/5.15.2/msvc2019_64/lib/cmake/Qt5Core
    set(_qt_root "")
    if(DEFINED Qt5Core_DIR)
        get_filename_component(_qt_root "${Qt5Core_DIR}/../../../" ABSOLUTE)
    elseif(DEFINED ENV{QT5_DIR})
        set(_qt_root "$ENV{QT5_DIR}")
    elseif(DEFINED ENV{QT_DIR})
        set(_qt_root "$ENV{QT_DIR}")
    endif()

    find_program(ENS_WINDEPLOYQT_EXECUTABLE
        NAMES windeployqt
        HINTS "${_qt_root}/bin"
        DOC "Path to Qt windeployqt deployment tool"
    )

    if(NOT ENS_WINDEPLOYQT_EXECUTABLE)
        message(WARNING "windeployqt not found; Qt runtime DLLs will not be deployed automatically.")
        return()
    endif()

    # 构造串行部署命令：每个目标一条 COMMAND，CMake 会按顺序执行（不并行），
    # 全部写入同一个 bin/<CONFIG> 目录，互不争抢。
    set(_deploy_cmds "")
    foreach(_t ${_targets})
        list(APPEND _deploy_cmds
            COMMAND "${CMAKE_COMMAND}" -E env
                "QTDIR=${_qt_root}"
                "PATH=${_qt_root}/bin;$ENV{PATH}"
                "${ENS_WINDEPLOYQT_EXECUTABLE}"
                --no-translations
                --no-compiler-runtime
                --no-opengl-sw
                --no-angle
                --dir "$<TARGET_FILE_DIR:${_t}>"
                "$<TARGET_FILE:${_t}>"
        )
    endforeach()

    add_custom_target(ens_deploy ALL
        ${_deploy_cmds}
        DEPENDS ${_targets}
        COMMENT "Deploying Qt runtime dependencies (serialized, single step)"
        VERBATIM
    )
endfunction()
