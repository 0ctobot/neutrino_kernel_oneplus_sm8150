ifneq ($(TARGET_PRODUCT),qssi)
ifeq ($(call is-board-platform-in-list, msmnile),true)
#Make file to create RMNET_SHS DLKM
LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)

LOCAL_CFLAGS := -Wno-macro-redefined -Wno-unused-function -Wall -Werror
LOCAL_CLANG :=true

LOCAL_MODULE_PATH := $(KERNEL_MODULES_OUT)
LOCAL_MODULE := rmnet_shs.ko

LOCAL_SRC_FILES := rmnet_shs_main.c rmnet_shs_config.c rmnet_shs_wq.c

RMNET_SHS_BLD_DIR := ../../vendor/qcom/opensource/data-kernel/drivers/rmnet/shs
DLKM_DIR := ./device/qcom/common/dlkm

KBUILD_OPTIONS := $(RMNET_SHS_BLD_DIR)
LOCAL_MODULE_TAGS := debug

$(warning $(DLKM_DIR))
include $(DLKM_DIR)/AndroidKernelModule.mk

endif #End of Check for target
endif #End of Check for qssi target
