# PocketMage app build recipe.
#
# Point at the SDK checkout first:
#   export PM_SDK_ROOT=/path/to/PocketMage_SDK
# or pass it per-invocation:
#   make SDK_ROOT=/path/to/PocketMage_SDK
#
# APP_NAME defaults to the directory name, so myapp/ builds myapp.app.elf.
# Rename the directory (or set APP_NAME here) and regenerate the icon to
# match the new card name.

SDK_ROOT ?= $(PM_SDK_ROOT)
APP_NAME ?= $(notdir $(CURDIR))

# The pack target ships $(APP_NAME)_ICON.bin; regenerate it from the PNG
# asset whenever the name or the artwork changes. Plain `make` (the ELF only)
# needs no Pillow; `make pack` does.
$(APP_NAME)_ICON.bin: assets/icon.png tools/make_icon.py
	python3 tools/make_icon.py assets/icon.png $@

include $(SDK_ROOT)/tools/app.mk

icon: $(APP_NAME)_ICON.bin