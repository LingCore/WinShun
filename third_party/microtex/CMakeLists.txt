# MicroTeX (MIT, https://github.com/NanoMichael/MicroTeX, branch openmath at
# 086f4eb7, 2024-08-06): lays out LaTeX's math with an OpenType math font, for
# 公式识别's preview and pictures (src/capture/FormulaPicture). lib/ as
# released, without its C wrapper and build files, but for two changes:
# microtexexport.h exports nothing with MICROTEX_STATIC (it had dllimport on
# MSVC), and \mathop takes its limits under and over it in display style as
# in TeX (macro/macro_types.h; \lim_{x \to 0} had them beside it). The
# drawing on QPainter is Win顺's own (FormulaPicture), not the project's
# platform/qt.
#
# The font is Latin Modern Math 1.959 (GUST Font License, res/) as MicroTeX's
# .clm2: its metrics with the glyphs' outlines, drawn as paths, so no font is
# installed or loaded (GLYPH_RENDER_TYPE 1). Words in \text{} are drawn with
# Windows' fonts.
#
# C++17 as it is written (u8"" is char in it); its own warnings are not ours.

add_library(microtex STATIC
    lib/atom/atom.cpp lib/atom/atom_basic.cpp lib/atom/atom_char.cpp lib/atom/atom_misc.cpp
    lib/atom/atom_matrix.cpp lib/atom/atom_row.cpp lib/atom/atom_space.cpp lib/atom/atom_stack.cpp
    lib/atom/atom_accent.cpp lib/atom/atom_scripts.cpp lib/atom/atom_vrow.cpp lib/atom/atom_operator.cpp
    lib/atom/atom_zstack.cpp lib/atom/atom_sideset.cpp lib/atom/atom_font.cpp lib/atom/atom_delim.cpp
    lib/atom/atom_root.cpp lib/atom/atom_frac.cpp lib/atom/atom_fence.cpp lib/atom/atom_box.cpp
    lib/atom/atom_text.cpp lib/atom/colors_def.cpp
    lib/box/box.cpp lib/box/box_factory.cpp lib/box/box_group.cpp lib/box/box_single.cpp
    lib/core/split.cpp lib/core/formula.cpp lib/core/formula_def.cpp lib/core/glue.cpp
    lib/core/localized_num.cpp lib/core/parser.cpp lib/core/debug_config.cpp
    lib/macro/macro.cpp lib/macro/macro_def.cpp lib/macro/macro_misc.cpp lib/macro/macro_scripts.cpp
    lib/macro/macro_accent.cpp lib/macro/macro_colors.cpp lib/macro/macro_space.cpp lib/macro/macro_delims.cpp
    lib/macro/macro_frac.cpp lib/macro/macro_styles.cpp lib/macro/macro_fonts.cpp
    lib/env/env.cpp lib/env/units.cpp
    lib/utils/string_utils.cpp lib/utils/utf.cpp lib/utils/utils.cpp
    lib/otf/clm.cpp lib/otf/fontsense.cpp lib/otf/glyph.cpp lib/otf/otf.cpp lib/otf/path.cpp
    lib/unimath/font_src.cpp lib/unimath/math_type.cpp lib/unimath/uni_char.cpp lib/unimath/uni_font.cpp
    lib/unimath/uni_symbol.cpp
    lib/graphic/font_style.cpp lib/graphic/graphic_basic.cpp lib/graphic/graphic.cpp
    lib/render/render.cpp lib/render/builder.cpp
    lib/microtex.cpp)
set(PROJECT_VERSION_MAJOR 1)
set(PROJECT_VERSION_MINOR 0)
set(PROJECT_VERSION_PATCH 0)
configure_file(lib/microtexconfig.h.in ${CMAKE_CURRENT_BINARY_DIR}/generated/microtexconfig.h @ONLY)
target_include_directories(microtex PUBLIC lib ${CMAKE_CURRENT_BINARY_DIR}/generated)
# Static, so nothing is exported; std::byte as the project has it.
target_compile_definitions(microtex PUBLIC MICROTEX_STATIC _HAS_STD_BYTE=1 PRIVATE GLYPH_RENDER_TYPE=1)
set_target_properties(microtex PROPERTIES
    CXX_STANDARD 17 AUTOMOC OFF AUTOUIC OFF AUTORCC OFF DISABLE_PRECOMPILE_HEADERS ON)
if(MSVC)
    target_compile_options(microtex PRIVATE /utf-8 /W0 /EHsc)
endif()
