#---------------------------------------------------------------------------
# krt_add_tool(<target> SOURCES <src...> [WEB_DIR <dir>] [LIBS <lib...>])
#
# ツール 1 本 (exe) を作る。画面 (web/) は次の 3 つを 1 つのフォルダへ
# 集めてから exe に埋め込む:
#
#   lib/appserve.js   … appserve の JS ランタイム (external/appserve/web/lib)
#   common/           … 全ツール共通の部品 (web/common: krt.js / krt.css)
#   (ツールの web/)    … ツール固有の画面 (index.html ほか)
#
# 集める処理は configure 時に行い、元ファイルを CMAKE_CONFIGURE_DEPENDS に
# 載せるので、web/ を編集すれば次のビルドで自動的に取り込み直される。
# 起動時は exe に埋め込んだ画面を最優先で使う (appserve。カレントに別の web/ があっても
# 影響しない)。開発中に exe を作り直さず画面だけ直したい場合は
# --web-root=<集めたフォルダ> (build/<preset>/tools/<tool>/web) を付けて起動する。
#---------------------------------------------------------------------------
# "cmake/.." のままだと同じファイルが 2 通りの綴りで依存に載り、ninja が
# «同じ出力が複数» で止まるので正規化しておく
get_filename_component(KRT_ROOT_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(KRT_ROOT_DIR "${KRT_ROOT_DIR}" CACHE INTERNAL "")

function(krt_add_tool target)
    cmake_parse_arguments(KAT "" "WEB_DIR" "SOURCES;LIBS" ${ARGN})

    add_executable(${target} ${KAT_SOURCES})
    target_link_libraries(${target} PRIVATE krt_app ${KAT_LIBS})

    if(KAT_WEB_DIR)
        set(_stage "${CMAKE_CURRENT_BINARY_DIR}/web")
        file(REMOVE_RECURSE "${_stage}")
        file(MAKE_DIRECTORY "${_stage}/lib" "${_stage}/common")

        set(_sources_root
            "${KRT_ROOT_DIR}/external/appserve/web/lib|lib"
            "${KRT_ROOT_DIR}/web/common|common"
            "${KAT_WEB_DIR}|.")
        foreach(_pair IN LISTS _sources_root)
            string(REPLACE "|" ";" _pair "${_pair}")
            list(GET _pair 0 _src)
            list(GET _pair 1 _dst)
            file(GLOB_RECURSE _files RELATIVE "${_src}" "${_src}/*")
            foreach(_f IN LISTS _files)
                configure_file("${_src}/${_f}" "${_stage}/${_dst}/${_f}" COPYONLY)
                set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_src}/${_f}")
            endforeach()
        endforeach()

        appserve_embed_web(${target} WEB_DIR "${_stage}" NAME "${target}_web")
    endif()

    if(WIN32)
        # コンソールから CLI として使うため、サブシステムはコンソールのまま。
        # GUI として起動したときのコンソール窓は krt::hideConsoleIfOwned() が隠す。
        set_target_properties(${target} PROPERTIES OUTPUT_NAME ${target})
    endif()
endfunction()
