# Copyright (c) 2024 Afiq Zudin Hadi
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

if(CONFIG_STENO_ENGINE)

target_include_directories(app PRIVATE
    include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# ── Split-dict mode ──────────────────────────────────
# Central: behavior engine + BLE dict client
# Peripheral: dict embed + lookup engine + GATT server
if(CONFIG_STENO_SPLIT_DICT)

  if(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    # Central side: behavior engine queries peripheral over BLE
    target_sources(app PRIVATE
        src/behavior_steno.c
        src/output.c
        src/formatter.c
        src/undo.c
        src/split_dict.c
        src/split_cache.c
    )
  else()
    # Peripheral side: dict embedded here, serves GATT queries
    target_sources(app PRIVATE
        src/dict_embed.S
    )
    if(CONFIG_STENO_DICT_MPHF)
      target_sources(app PRIVATE src/dict_mphf.c)
    else()
      target_sources(app PRIVATE src/trie.c)
    endif()
  endif()

# ── Non-split mode ───────────────────────────────────
# Everything on one board (or central-only without split)
else()
  if(NOT CONFIG_ZMK_SPLIT OR CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    target_sources(app PRIVATE
        src/behavior_steno.c
        src/output.c
        src/formatter.c
        src/undo.c
        src/dict_embed.S
    )
    if(CONFIG_STENO_DICT_MPHF)
      target_sources(app PRIVATE src/dict_mphf.c)
    else()
      target_sources(app PRIVATE src/trie.c)
    endif()
  endif()
endif()

# ── Dictionary compilation ───────────────────────────
# Build dict binary when we embed it (non-split, or split peripheral)
set(STENO_NEED_DICT_EMBED FALSE)
if(CONFIG_STENO_SPLIT_DICT)
  if(NOT CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    set(STENO_NEED_DICT_EMBED TRUE)
  endif()
elseif(NOT CONFIG_ZMK_SPLIT OR CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
  set(STENO_NEED_DICT_EMBED TRUE)
endif()

if(STENO_NEED_DICT_EMBED)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  set(STENO_DICT_BIN ${CMAKE_CURRENT_BINARY_DIR}/steno_dict.bin)
  set(STENO_DICTS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/dicts)
  set(STENO_FETCH ${CMAKE_CURRENT_SOURCE_DIR}/tools/fetch_dict.py)

  if(CONFIG_STENO_DICT_PLOVER)
    set(STENO_DICT_NAME "plover")
    set(STENO_DICT_SRC ${STENO_DICTS_DIR}/plover-main.json)
  elseif(CONFIG_STENO_DICT_LAPWING)
    set(STENO_DICT_NAME "lapwing")
    set(STENO_DICT_SRC ${STENO_DICTS_DIR}/lapwing.json)
  else()
    set(STENO_DICT_SRC ${STENO_DICTS_DIR}/test.json)
  endif()

  # Auto-download dict if needed (Plover/Lapwing only)
  if(DEFINED STENO_DICT_NAME AND NOT EXISTS ${STENO_DICT_SRC})
    message(STATUS "Steno: downloading ${STENO_DICT_NAME} dictionary...")
    execute_process(
        COMMAND ${Python3_EXECUTABLE} ${STENO_FETCH}
            ${STENO_DICT_NAME} ${STENO_DICTS_DIR}
        RESULT_VARIABLE FETCH_RESULT
    )
    if(NOT FETCH_RESULT EQUAL 0)
      message(WARNING "Steno: dict download failed. Build may fail.")
    endif()
  endif()

  # Compile dict binary
  if(EXISTS ${STENO_DICT_SRC})
    if(CONFIG_STENO_DICT_MPHF)
      # Fetch at build time if hash changed
      if(DEFINED STENO_DICT_NAME)
        add_custom_command(
            OUTPUT ${STENO_DICT_SRC}.stamp
            COMMAND ${Python3_EXECUTABLE} ${STENO_FETCH}
                ${STENO_DICT_NAME} ${STENO_DICTS_DIR}
            COMMAND ${CMAKE_COMMAND} -E touch ${STENO_DICT_SRC}.stamp
            COMMENT "Checking ${STENO_DICT_NAME} dictionary for updates"
        )
        add_custom_target(steno_dict_fetch DEPENDS ${STENO_DICT_SRC}.stamp)
      endif()

      add_custom_command(
          OUTPUT ${STENO_DICT_BIN}
          COMMAND ${Python3_EXECUTABLE}
              ${CMAKE_CURRENT_SOURCE_DIR}/tools/compile_mphf.py
              ${STENO_DICT_SRC} ${STENO_DICT_BIN}
              --max-size ${CONFIG_STENO_DICT_MAX_SIZE}
          DEPENDS ${STENO_DICT_SRC}
              ${CMAKE_CURRENT_SOURCE_DIR}/tools/compile_mphf.py
          COMMENT "Compiling steno dictionary (MPHF)"
      )
    else()
      add_custom_command(
          OUTPUT ${STENO_DICT_BIN}
          COMMAND ${Python3_EXECUTABLE}
              ${CMAKE_CURRENT_SOURCE_DIR}/tools/compile_simple.py
              ${STENO_DICT_SRC} -o ${STENO_DICT_BIN}
          DEPENDS ${STENO_DICT_SRC}
              ${CMAKE_CURRENT_SOURCE_DIR}/tools/compile_simple.py
          COMMENT "Compiling steno dictionary (simple)"
      )
    endif()

    add_custom_target(steno_dict_gen DEPENDS ${STENO_DICT_BIN})
    if(TARGET steno_dict_fetch)
      add_dependencies(steno_dict_gen steno_dict_fetch)
    endif()
    add_dependencies(app steno_dict_gen)

    set_property(SOURCE src/dict_embed.S APPEND PROPERTY
        COMPILE_DEFINITIONS STENO_DICT_BIN_PATH="${STENO_DICT_BIN}")
  endif()
endif()

endif() # CONFIG_STENO_ENGINE
