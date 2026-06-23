# Rockchip Recovery — Linux SDK build
# Usage:
#   make RecoveryNoUi=true
#   make RECOVERY_FEATURES="RK_STORAGE_EMMC RK_STORAGE_MTD RK_SOURCE_SD ..."
#
# Feature switches are definition-based: a name present in RECOVERY_FEATURES is
# compiled as -D<name>, and absent means disabled. Old command-line overrides
# such as RK_STORAGE_MTD=0/1 are accepted only as a compatibility shim.
# RecoveryNoUi accepts 1/y/yes/true/on for old Buildroot packages.

CC      ?= $(CROSS_COMPILE)gcc

# Buildroot's pkg-generic.mk overrides CFLAGS via `make CFLAGS="..."`, which
# wipes anything we put into CFLAGS here. Use a separate variable for our
# project-local include paths and feature defines, and prepend it in the
# compile rule so it can't be clobbered by external CFLAGS overrides.
LOCAL_CFLAGS := -Wall -Wextra -Iinclude -D_FILE_OFFSET_BITS=64

# Same rationale for link flags: buildroot also overrides LDFLAGS via
# `make LDFLAGS=""`. Keep project-required link libs in LOCAL_LDLIBS and
# append them at link time so external LDFLAGS overrides can't drop them.
LOCAL_LDLIBS := -lpthread

# ---- Feature switches ----
RECOVERY_DEFAULT_FEATURES := \
  RK_STORAGE_EMMC RK_STORAGE_MTD RK_STORAGE_UFS \
  RK_SOURCE_SD RK_SOURCE_USB RK_SOURCE_URL \
  RK_DIFF_UPDATE RK_GPT_UPDATE RK_IDBLOCK_UPDATE \
  RK_AB_SUPPORT

RECOVERY_FEATURES ?= $(RECOVERY_DEFAULT_FEATURES)
RECOVERY_DISABLE_FEATURES ?=

false_values := 0 n no false off

define apply_legacy_feature
ifneq ($$(origin $(1)),undefined)
  ifneq ($$(filter $$(false_values),$$(strip $$($(1)))),)
    RECOVERY_DISABLE_FEATURES += $(1)
  else
    RECOVERY_FEATURES += $(1)
  endif
endif
endef

$(eval $(call apply_legacy_feature,RK_STORAGE_EMMC))
$(eval $(call apply_legacy_feature,RK_STORAGE_MTD))
$(eval $(call apply_legacy_feature,RK_STORAGE_UFS))
$(eval $(call apply_legacy_feature,RK_SOURCE_SD))
$(eval $(call apply_legacy_feature,RK_SOURCE_USB))
$(eval $(call apply_legacy_feature,RK_SOURCE_URL))
$(eval $(call apply_legacy_feature,RK_DIFF_UPDATE))
$(eval $(call apply_legacy_feature,RK_GPT_UPDATE))
$(eval $(call apply_legacy_feature,RK_IDBLOCK_UPDATE))
$(eval $(call apply_legacy_feature,RK_AB_SUPPORT))

ifneq ($(origin RK_AB_RETRY_BOOT),undefined)
  ifeq ($(filter $(false_values),$(strip $(RK_AB_RETRY_BOOT))),)
    RECOVERY_FEATURES += RETRY_BOOT
  endif
endif
ifneq ($(origin RETRY_BOOT),undefined)
  ifeq ($(filter $(false_values),$(strip $(RETRY_BOOT))),)
    RECOVERY_FEATURES += RETRY_BOOT
  endif
endif
ifneq ($(origin SUCCESSFUL_BOOT),undefined)
  ifeq ($(filter $(false_values),$(strip $(SUCCESSFUL_BOOT))),)
    RECOVERY_FEATURES += SUCCESSFUL_BOOT
  endif
endif

RECOVERY_FEATURES := $(sort $(filter-out $(RECOVERY_DISABLE_FEATURES),$(RECOVERY_FEATURES)))

LOCAL_CFLAGS += $(addprefix -D,$(filter RK_%,$(RECOVERY_FEATURES)))

ifneq ($(filter RK_DIFF_UPDATE,$(RECOVERY_FEATURES)),)
  LOCAL_LDLIBS += -lbz2
endif
ifneq ($(filter RK_AB_SUPPORT,$(RECOVERY_FEATURES)),)
  ifneq ($(filter RETRY_BOOT,$(RECOVERY_FEATURES)),)
    LOCAL_CFLAGS += -DRETRY_BOOT
  else ifneq ($(filter SUCCESSFUL_BOOT,$(RECOVERY_FEATURES)),)
    LOCAL_CFLAGS += -DSUCCESSFUL_BOOT
  else ifneq ($(findstring -DRETRY_BOOT,$(CFLAGS)),)
  else ifneq ($(findstring -DSUCCESSFUL_BOOT,$(CFLAGS)),)
  else
    LOCAL_CFLAGS += -DSUCCESSFUL_BOOT
  endif
endif
ifneq ($(filter RK_SOURCE_URL,$(RECOVERY_FEATURES)),)
  # download.o (core/, always compiled) is linked into BOTH binaries —
  # recovery's source_ota.c calls download_file too — so curl is a common
  # link dep, not engine-only.
  LOCAL_LDLIBS += -lcurl
endif

# ---- Common objects ----
UTILS_OBJ = utils/log.o utils/strlcpy.o utils/strlcat.o utils/process.o \
            utils/cmdline.o

# device probing + mtd info are storage-domain (live in storage/); always
# compiled (device_is_mtd / mtd_parse_dev_num are used even in non-MTD builds).
STORAGE_OBJ = storage/storage.o storage/device.o storage/mtd_info.o \
              storage/fstab.o storage/mount.o storage/format.o storage/mounts.o
ifneq ($(filter RK_STORAGE_EMMC RK_STORAGE_UFS,$(RECOVERY_FEATURES)),)
  STORAGE_OBJ += storage/block_io.o
endif
ifneq ($(filter RK_STORAGE_MTD,$(RECOVERY_FEATURES)),)
  STORAGE_OBJ += storage/mtd.o storage/ubi.o
endif

CORE_OBJ = core/bootmsg.o core/slot.o core/image.o \
           core/checksum.o core/md5.o core/flash.o \
           core/download.o
ifneq ($(filter RK_IDBLOCK_UPDATE,$(RECOVERY_FEATURES)),)
  CORE_OBJ += core/flash_bootloader.o
endif
ifneq ($(filter RK_GPT_UPDATE,$(RECOVERY_FEATURES)),)
  CORE_OBJ += core/flash_parameter.o
endif
ifneq ($(filter RK_DIFF_UPDATE,$(RECOVERY_FEATURES)),)
  CORE_OBJ += core/patch.o core/flash_diff.o
endif
ifneq ($(filter RK_SOURCE_URL,$(RECOVERY_FEATURES)),)
  LOCAL_CFLAGS += -I$(shell pkg-config --variable=includedir libcurl 2>/dev/null || echo /usr/include)
endif

COMMON_OBJ = $(UTILS_OBJ) $(STORAGE_OBJ) $(CORE_OBJ)

# ---- updateEngine ----
ENGINE_OBJ = app/engine/engine_main.o app/engine/cmd_upgrade.o \
             app/engine/upgrade_resolver.o app/engine/upgrade_plan.o \
             app/engine/upgrade_precheck.o app/engine/upgrade_version.o \
             app/engine/cmd_misc.o app/engine/cmd_slot.o \
             app/engine/engine_cmd.o app/engine/upgrade_filter.o \
             app/common/source.o app/common/source_ota.o

# ---- recovery ----
RECOVERY_OBJ = app/recovery/recovery_main.o app/recovery/recovery_cmd.o \
               app/common/source.o app/common/source_ota.o ui/menu.o
ifneq ($(filter 1 y yes true on,$(strip $(RecoveryNoUi))),)
  RECOVERY_OBJ += ui/ui_null.o
else
  RECOVERY_OBJ += ui/ui.o ui/minui/events.o ui/minui/graphics.o \
                  ui/minui/graphics_drm.o ui/minui/graphics_fbdev.o \
                  ui/minui/resources.o \
                  app/recovery/prompt.o
  LOCAL_CFLAGS += -DRK_UI_ENABLE -Iui
  # -lz: libpng pulls zlib transitively for shared link; required explicitly
  # for static link. -lm reserved if minui ever uses math (not today).
  LOCAL_LDLIBS_RECOVERY += -ldrm -lpng -lz
endif

# ---- Targets ----
all: updateEngine recovery

updateEngine: $(ENGINE_OBJ) $(COMMON_OBJ)
	$(CC) -o $@ $^ $(LDFLAGS) $(LOCAL_LDLIBS) $(LOCAL_LDLIBS_ENGINE) $(EXTRA_LDLIBS)

recovery: $(RECOVERY_OBJ) $(COMMON_OBJ)
	$(CC) -o $@ $^ $(LDFLAGS) $(LOCAL_LDLIBS) $(LOCAL_LDLIBS_RECOVERY) $(EXTRA_LDLIBS)

%.o: %.c
	$(CC) $(LOCAL_CFLAGS) $(CFLAGS) -c -o $@ $<

clean:
	rm -f updateEngine recovery
	find . -name '*.o' -delete

.PHONY: all clean
