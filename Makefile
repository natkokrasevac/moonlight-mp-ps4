# check if Toolchain env variable is set
ifndef OO_PS4_TOOLCHAIN
$(error OO_PS4_TOOLCHAIN is not set. See README.md "Toolchain" section.)
endif

# Package metadata (sourced from config/config_pkg)
include config/config_pkg
# FTP upload config (sourced from config/config_ftp)
include config/config_ftp

# Directories
SRC_DIR     := src
BUILD_DIR   := _build
PKG_DIR     := pkg_template

PROJECT_NAME := $(shell basename $(CURDIR))

SRCS        := $(shell find $(SRC_DIR) \( -name "*.c" -o -name "*.cpp" -o -name "*.h" -o -name "*.hpp" \))

# Package contents (full paths for dependencies)
ASSETS_FULL   := $(shell find $(PKG_DIR)/assets -type f -not -name '.gitkeep' -not -name '.DS_Store')
LIBMODULES_FULL := $(shell find $(PKG_DIR)/sce_module -type f)

# Relative to PKG_DIR (for create-gp4 --files)
ASSETS      := $(patsubst $(PKG_DIR)/%,%,$(ASSETS_FULL))
LIBMODULES  := $(patsubst $(PKG_DIR)/%,%,$(LIBMODULES_FULL))

# Toolchain Paths
UNAME_S     := $(shell uname -s)
ifeq ($(UNAME_S),Linux)
    CDIR := linux
else ifeq ($(UNAME_S),Darwin)
    CDIR := macos
else
$(error Unsupported OS: $(UNAME_S). Only Linux and macOS are supported at this time.)
endif

TOOLCHAIN_FILE := cmake/toolchain-ps4-$(CDIR).cmake
TOOLS_DIR    := $(OO_PS4_TOOLCHAIN)/bin/$(CDIR)

.PHONY: all clean ftp

# Default target: build the pkg only (no FTP upload)
all: $(CONTENT_ID).pkg

# FTP upload target: build the pkg if needed, then upload it
ftp: $(CONTENT_ID).pkg
	# Upload and then immediately list the directory to confirm
	curl -T $(CONTENT_ID).pkg ftp://$(FTP_HOST):$(FTP_PORT)$(FTP_PATH) && curl ftp://$(FTP_HOST):$(FTP_PORT)$(FTP_PATH)

$(CONTENT_ID).pkg: pkg.gp4
	cd $(PKG_DIR) && $(TOOLS_DIR)/PkgTool.Core pkg_build pkg.gp4 ..

pkg.gp4: $(PKG_DIR)/pkg.gp4
	cp $(PKG_DIR)/pkg.gp4 .

$(PKG_DIR)/pkg.gp4: eboot.bin $(PKG_DIR)/sce_sys/param.sfo $(PKG_DIR)/sce_sys/icon0.png $(PKG_DIR)/sce_sys/about/right.sprx $(LIBMODULES_FULL) $(ASSETS_FULL)
	cp eboot.bin $(PKG_DIR)/
	cd $(PKG_DIR) && $(TOOLS_DIR)/create-gp4 -out pkg.gp4 --content-id=$(CONTENT_ID) --files "eboot.bin sce_sys/param.sfo sce_sys/icon0.png sce_sys/about/right.sprx $(LIBMODULES) $(ASSETS)"
$(PKG_DIR)/sce_sys/param.sfo: config/config_pkg
	$(TOOLS_DIR)/PkgTool.Core sfo_new $@
	$(TOOLS_DIR)/PkgTool.Core sfo_setentry $@ APP_TYPE --type Integer --maxsize 4 --value 1
	$(TOOLS_DIR)/PkgTool.Core sfo_setentry $@ APP_VER --type Utf8 --maxsize 8 --value '$(VERSION)'
	$(TOOLS_DIR)/PkgTool.Core sfo_setentry $@ ATTRIBUTE --type Integer --maxsize 4 --value 0x00000020
	$(TOOLS_DIR)/PkgTool.Core sfo_setentry $@ CATEGORY --type Utf8 --maxsize 4 --value 'gd'
	$(TOOLS_DIR)/PkgTool.Core sfo_setentry $@ CONTENT_ID --type Utf8 --maxsize 48 --value '$(CONTENT_ID)'
	$(TOOLS_DIR)/PkgTool.Core sfo_setentry $@ DOWNLOAD_DATA_SIZE --type Integer --maxsize 4 --value 0
	$(TOOLS_DIR)/PkgTool.Core sfo_setentry $@ SYSTEM_VER --type Integer --maxsize 4 --value 0
	$(TOOLS_DIR)/PkgTool.Core sfo_setentry $@ TITLE --type Utf8 --maxsize 128 --value '$(TITLE)'
	$(TOOLS_DIR)/PkgTool.Core sfo_setentry $@ TITLE_ID --type Utf8 --maxsize 12 --value '$(TITLE_ID)'
	$(TOOLS_DIR)/PkgTool.Core sfo_setentry $@ VERSION --type Utf8 --maxsize 8 --value '$(VERSION)'

eboot.bin: $(BUILD_DIR)/$(PROJECT_NAME).elf
	$(TOOLS_DIR)/create-fself -in=$< -out=$(BUILD_DIR)/$(PROJECT_NAME).oelf --eboot "eboot.bin" --paid 0x3800000000000011

$(BUILD_DIR)/$(PROJECT_NAME).elf: $(SRCS) CMakeLists.txt
	cmake -B $(BUILD_DIR) -DCMAKE_TOOLCHAIN_FILE=$(TOOLCHAIN_FILE)
	cmake --build $(BUILD_DIR)

clean:
	rm -rf $(BUILD_DIR) eboot.bin $(CONTENT_ID).pkg pkg.gp4 $(PKG_DIR)/sce_sys/param.sfo $(PKG_DIR)/eboot.bin $(PKG_DIR)/pkg.gp4


