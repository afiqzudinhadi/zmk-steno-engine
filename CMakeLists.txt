# Copyright (c) 2024 Afiq Zudin Hadi
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

if(CONFIG_STENO_ENGINE)

target_include_directories(app PRIVATE
    include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(STENO_DICTS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/dicts)
set(STENO_FETCH ${CMAKE_CURRENT_SOURCE_DIR}/tools/fetch_dict.py)

# ── Dictionary selection ─────────────────────────────
# Maps the Kconfig choice onto the compiler's --dicts argument.
if(CONFIG_STENO_DICT_PLOVER)
  set(STENO_DICTS_ARG "plover")
elseif(CONFIG_STENO_DICT_LAPWING)
  set(STENO_DICTS_ARG "lapwing")
else()
  set(STENO_DICTS_ARG "both")
endif()

# Ensure a dictionary JSON is present: download at configure time if
# missing (hard error on failure), and add a build-time stamp rule that
# re-checks the upstream hash on fresh build directories.
function(steno_fetch_dict name json_path)
  if(NOT EXISTS ${json_path})
    message(STATUS "Steno: downloading ${name} dictionary...")
    execute_process(
        COMMAND ${Python3_EXECUTABLE} ${STENO_FETCH} ${name} ${STENO_DICTS_DIR}
        RESULT_VARIABLE FETCH_RESULT
    )
    if(NOT FETCH_RESULT EQUAL 0 OR NOT EXISTS ${json_path})
      message(FATAL_ERROR
          "Steno: ${name} dictionary is missing and the download failed "
          "(expected ${json_path}).")
    endif()
  endif()
  add_custom_command(
      OUTPUT ${json_path}.stamp
      COMMAND ${Python3_EXECUTABLE} ${STENO_FETCH} ${name} ${STENO_DICTS_DIR}
      COMMAND ${CMAKE_COMMAND} -E touch ${json_path}.stamp
      COMMENT "Checking ${name} dictionary for updates"
  )
endfunction()

# ── v4 union split-section format ────────────────────
# One compiler run emits BOTH half blobs (docs/FORMAT_V4.md §5).
# Left (central): DISP, MEMBERSHIP, FP, CONFLICTS, VALIDX slice [0, k).
# Right (peripheral): STRDIR, STRINGS, CONFLICTS, VALIDX slice [k, n).
if(CONFIG_STENO_DICT_V4)

  if(NOT CONFIG_STENO_SPLIT_DICT)
    message(FATAL_ERROR
        "STENO_DICT_V4 requires STENO_SPLIT_DICT: the v4 format spans both "
        "keyboard halves (left = lookup structures, right = string table). "
        "Enable STENO_SPLIT_DICT, or disable STENO_DICT_V4 to fall back to "
        "the legacy MPHF format.")
  endif()

  set(STENO_V4_LEFT_BIN ${CMAKE_CURRENT_BINARY_DIR}/steno_v4_left.bin)
  set(STENO_V4_RIGHT_BIN ${CMAKE_CURRENT_BINARY_DIR}/steno_v4_right.bin)

  if(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    # Central: behavior engine + left half blob + BLE client
    target_sources(app PRIVATE
        src/behavior_steno.c
        src/output.c
        src/formatter.c
        src/undo.c
        src/dict_v4.c
        src/split_dict.c
        src/split_cache.c
        src/dict_embed.S
    )
    set(STENO_DICT_BIN ${STENO_V4_LEFT_BIN})
  else()
    # Peripheral: right half blob + GATT server
    target_sources(app PRIVATE
        src/dict_v4.c
        src/split_dict.c
        src/split_cache.c
        src/dict_embed.S
    )
    set(STENO_DICT_BIN ${STENO_V4_RIGHT_BIN})
  endif()

  # The union format is always compiled from both source dicts;
  # --dicts controls which of them are included (dicts_mask).
  set(STENO_PLOVER_JSON ${STENO_DICTS_DIR}/plover-main.json)
  set(STENO_LAPWING_JSON ${STENO_DICTS_DIR}/lapwing.json)
  steno_fetch_dict(plover ${STENO_PLOVER_JSON})
  steno_fetch_dict(lapwing ${STENO_LAPWING_JSON})

  if(CONFIG_STENO_DEBUG_NO_EMBED)
    # Bisect aid: no blob, dict_embed.S falls back to a 4-byte stub.
    set(STENO_DICT_BIN "")
  else()

  # Single run produces both blobs. The compiler FAILS the build if
  # either half exceeds its budget — entries are never trimmed.
  add_custom_command(
      OUTPUT ${STENO_V4_LEFT_BIN} ${STENO_V4_RIGHT_BIN}
      COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/tools/compile_v4.py
          --plover ${STENO_PLOVER_JSON} --lapwing ${STENO_LAPWING_JSON}
          --dicts ${STENO_DICTS_ARG}
          --out-dir ${CMAKE_CURRENT_BINARY_DIR}
          --left-size ${CONFIG_STENO_DICT_LEFT_MAX_SIZE}
          --right-size ${CONFIG_STENO_DICT_RIGHT_MAX_SIZE}
      DEPENDS
          ${STENO_PLOVER_JSON} ${STENO_LAPWING_JSON}
          ${STENO_PLOVER_JSON}.stamp ${STENO_LAPWING_JSON}.stamp
          ${CMAKE_CURRENT_SOURCE_DIR}/tools/compile_v4.py
      COMMENT "Compiling steno v4 union dictionary (left + right half blobs)"
  )

  add_custom_target(steno_dict_gen DEPENDS ${STENO_DICT_BIN})
  add_dependencies(app steno_dict_gen)

  endif() # CONFIG_STENO_DEBUG_NO_EMBED

# ── Legacy MPHF format (STENO_DICT_V4=n fallback) ────
else()

  if(CONFIG_STENO_DICT_BOTH)
    message(FATAL_ERROR
        "STENO_DICT_BOTH requires STENO_DICT_V4: the legacy MPHF format "
        "holds a single dictionary. Select STENO_DICT_PLOVER or "
        "STENO_DICT_LAPWING, or re-enable STENO_DICT_V4.")
  endif()

  if(CONFIG_STENO_DICT_PLOVER)
    set(STENO_DICT_NAME "plover")
    set(STENO_DICT_SRC ${STENO_DICTS_DIR}/plover-main.json)
  else()
    set(STENO_DICT_NAME "lapwing")
    set(STENO_DICT_SRC ${STENO_DICTS_DIR}/lapwing.json)
  endif()

  if(CONFIG_STENO_SPLIT_DICT)
    if(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
      # Central: behavior engine + local dict + BLE client
      target_sources(app PRIVATE
          src/behavior_steno.c
          src/output.c
          src/formatter.c
          src/undo.c
          src/dict_embed.S
          src/dict_mphf.c
          src/split_dict.c
          src/split_cache.c
      )
    else()
      # Peripheral: local dict + GATT server for central queries
      target_sources(app PRIVATE
          src/dict_embed.S
          src/dict_mphf.c
          src/split_dict.c
          src/split_cache.c
      )
    endif()
  elseif(NOT CONFIG_ZMK_SPLIT OR CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    target_sources(app PRIVATE
        src/behavior_steno.c
        src/output.c
        src/formatter.c
        src/undo.c
        src/dict_embed.S
        src/dict_mphf.c
    )
  endif()

  # Dict embed needed on: both halves in split mode, else central /
  # non-split only.
  set(STENO_NEED_DICT_EMBED FALSE)
  if(CONFIG_STENO_SPLIT_DICT)
    set(STENO_NEED_DICT_EMBED TRUE)
  elseif(NOT CONFIG_ZMK_SPLIT OR CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    set(STENO_NEED_DICT_EMBED TRUE)
  endif()

  if(STENO_NEED_DICT_EMBED)
    set(STENO_DICT_BIN ${CMAKE_CURRENT_BINARY_DIR}/steno_dict.bin)
    steno_fetch_dict(${STENO_DICT_NAME} ${STENO_DICT_SRC})

    if(CONFIG_STENO_SPLIT_DICT)
      if(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
        set(STENO_SPLIT_PART "left")
        set(STENO_SPLIT_SIZE ${CONFIG_STENO_DICT_LEFT_MAX_SIZE})
      else()
        set(STENO_SPLIT_PART "right")
        set(STENO_SPLIT_SIZE ${CONFIG_STENO_DICT_RIGHT_MAX_SIZE})
      endif()

      add_custom_command(
          OUTPUT ${STENO_DICT_BIN}
          COMMAND ${Python3_EXECUTABLE}
              ${CMAKE_CURRENT_SOURCE_DIR}/tools/compile_mphf.py
              ${STENO_DICT_SRC} ${STENO_DICT_BIN}
              --split-part ${STENO_SPLIT_PART}
              --left-size ${CONFIG_STENO_DICT_LEFT_MAX_SIZE}
              --right-size ${CONFIG_STENO_DICT_RIGHT_MAX_SIZE}
              --max-size ${STENO_SPLIT_SIZE}
              --block-size 2048
          DEPENDS ${STENO_DICT_SRC} ${STENO_DICT_SRC}.stamp
              ${CMAKE_CURRENT_SOURCE_DIR}/tools/compile_mphf.py
          COMMENT "Compiling steno dictionary (${STENO_SPLIT_PART} partition)"
      )
    else()
      add_custom_command(
          OUTPUT ${STENO_DICT_BIN}
          COMMAND ${Python3_EXECUTABLE}
              ${CMAKE_CURRENT_SOURCE_DIR}/tools/compile_mphf.py
              ${STENO_DICT_SRC} ${STENO_DICT_BIN}
              --max-size ${CONFIG_STENO_DICT_MAX_SIZE}
          DEPENDS ${STENO_DICT_SRC} ${STENO_DICT_SRC}.stamp
              ${CMAKE_CURRENT_SOURCE_DIR}/tools/compile_mphf.py
          COMMENT "Compiling steno dictionary (MPHF)"
      )
    endif()

    add_custom_target(steno_dict_gen DEPENDS ${STENO_DICT_BIN})
    add_dependencies(app steno_dict_gen)
  endif()

endif() # CONFIG_STENO_DICT_V4

# ── Dict embed plumbing ──────────────────────────────
# dict_embed.S .incbin's the blob named by the generated header;
# OBJECT_DEPENDS makes it re-assemble when the blob changes.
if(STENO_DICT_BIN)
  file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/steno_dict_path.h
       "#define STENO_DICT_BIN_PATH \"${STENO_DICT_BIN}\"\n")
  set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/src/dict_embed.S
      TARGET_DIRECTORY app
      PROPERTIES OBJECT_DEPENDS ${STENO_DICT_BIN})
else()
  # Debug no-embed: header exists but defines no blob path;
  # dict_embed.S falls back to a 4-byte stub.
  file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/steno_dict_path.h
       "/* STENO_DEBUG_NO_EMBED: no dictionary blob */\n")
endif()
target_include_directories(app PRIVATE ${CMAKE_CURRENT_BINARY_DIR})

endif() # CONFIG_STENO_ENGINE
