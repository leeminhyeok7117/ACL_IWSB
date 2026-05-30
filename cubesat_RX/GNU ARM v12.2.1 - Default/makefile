################################################################################
# Automatically-generated file. Do not edit!
################################################################################

-include ../makefile.init

RM := rm -rf

# All of the sources participating in the build are defined here
-include sources.mk
-include gecko_sdk_4.5.0/util/third_party/printf/src/subdir.mk
-include gecko_sdk_4.5.0/util/third_party/printf/subdir.mk
-include gecko_sdk_4.5.0/util/third_party/mbedtls/library/subdir.mk
-include gecko_sdk_4.5.0/util/silicon_labs/silabs_core/memory_manager/subdir.mk
-include gecko_sdk_4.5.0/protocol/flex/stack/framework/subdir.mk
-include gecko_sdk_4.5.0/protocol/flex/stack/core/subdir.mk
-include gecko_sdk_4.5.0/protocol/flex/stack/config/subdir.mk
-include gecko_sdk_4.5.0/protocol/flex/poll/subdir.mk
-include gecko_sdk_4.5.0/protocol/flex/ota-unicast-bootloader/ota-unicast-bootloader-client/subdir.mk
-include gecko_sdk_4.5.0/protocol/flex/app-framework-common/subdir.mk
-include gecko_sdk_4.5.0/platform/service/udelay/src/subdir.mk
-include gecko_sdk_4.5.0/platform/service/token_manager/src/subdir.mk
-include gecko_sdk_4.5.0/platform/service/system/src/subdir.mk
-include gecko_sdk_4.5.0/platform/service/sleeptimer/src/subdir.mk
-include gecko_sdk_4.5.0/platform/service/power_manager/src/subdir.mk
-include gecko_sdk_4.5.0/platform/service/mpu/src/subdir.mk
-include gecko_sdk_4.5.0/platform/service/iostream/src/subdir.mk
-include gecko_sdk_4.5.0/platform/service/device_init/src/subdir.mk
-include gecko_sdk_4.5.0/platform/service/cli/src/subdir.mk
-include gecko_sdk_4.5.0/platform/security/sl_component/sl_psa_driver/src/subdir.mk
-include gecko_sdk_4.5.0/platform/security/sl_component/sl_mbedtls_support/src/subdir.mk
-include gecko_sdk_4.5.0/platform/radio/rail_lib/plugin/rail_util_pti/subdir.mk
-include gecko_sdk_4.5.0/platform/radio/rail_lib/plugin/rail_util_power_manager_init/subdir.mk
-include gecko_sdk_4.5.0/platform/radio/rail_lib/plugin/pa-conversions/subdir.mk
-include gecko_sdk_4.5.0/platform/emlib/src/subdir.mk
-include gecko_sdk_4.5.0/platform/emdrv/nvm3/src/subdir.mk
-include gecko_sdk_4.5.0/platform/emdrv/gpiointerrupt/src/subdir.mk
-include gecko_sdk_4.5.0/platform/emdrv/dmadrv/src/subdir.mk
-include gecko_sdk_4.5.0/platform/driver/i2cspm/src/subdir.mk
-include gecko_sdk_4.5.0/platform/driver/debug/src/subdir.mk
-include gecko_sdk_4.5.0/platform/driver/button/src/subdir.mk
-include gecko_sdk_4.5.0/platform/common/toolchain/src/subdir.mk
-include gecko_sdk_4.5.0/platform/common/src/subdir.mk
-include gecko_sdk_4.5.0/platform/bootloader/app_properties/subdir.mk
-include gecko_sdk_4.5.0/platform/bootloader/api/subdir.mk
-include gecko_sdk_4.5.0/platform/Device/SiliconLabs/EFR32FG12P/Source/subdir.mk
-include gecko_sdk_4.5.0/hardware/driver/si70xx/src/subdir.mk
-include gecko_sdk_4.5.0/hardware/driver/mx25_flash_shutdown/src/sl_mx25_flash_shutdown_usart/subdir.mk
-include gecko_sdk_4.5.0/hardware/driver/configuration_over_swo/src/subdir.mk
-include gecko_sdk_4.5.0/hardware/board/src/subdir.mk
-include gecko_sdk_4.5.0/app/flex/component/connect/sl_connect_sdk_ota_bootloader_interface/subdir.mk
-include gecko_sdk_4.5.0/app/common/util/app_log/subdir.mk
-include autogen/subdir.mk
-include subdir.mk
-include objects.mk

ifneq ($(MAKECMDGOALS),clean)
ifneq ($(strip $(C_DEPS)),)
-include $(C_DEPS)
endif
endif

-include ../makefile.defs

OPTIONAL_TOOL_DEPS := \
$(wildcard ../makefile.defs) \
$(wildcard ../makefile.init) \
$(wildcard ../makefile.targets) \


BUILD_ARTIFACT_NAME := cubesat_RX
BUILD_ARTIFACT_EXTENSION := axf
BUILD_ARTIFACT_PREFIX :=
BUILD_ARTIFACT := $(BUILD_ARTIFACT_PREFIX)$(BUILD_ARTIFACT_NAME)$(if $(BUILD_ARTIFACT_EXTENSION),.$(BUILD_ARTIFACT_EXTENSION),)

# Add inputs and outputs from these tool invocations to the build variables 

# All Target
all: main-build

# Main-build Target
main-build: cubesat_RX.axf

# Tool invocations
cubesat_RX.axf: $(OBJS) $(USER_OBJS) makefile objects.mk $(OPTIONAL_TOOL_DEPS)
	@echo 'Building target: $@'
	@echo 'Invoking: GNU ARM C Linker'
	arm-none-eabi-gcc -g -gdwarf-2 -mcpu=cortex-m4 -mthumb -T "/home/lmh/SimplicityStudio/v5_workspace/cubesat_RX/autogen/linkerfile.ld" -Wl,--no-warn-rwx-segments -Xlinker --gc-sections -Xlinker -Map="cubesat_RX.map" -mfpu=fpv4-sp-d16 -mfloat-abi=softfp --specs=nano.specs -o cubesat_RX.axf -Wl,--start-group "./app_cli.o" "./app_init.o" "./app_process.o" "./fw_guard.o" "./main.o" "./autogen/app_framework_event_table.o" "./autogen/callback_dispatcher.o" "./autogen/rail_config.o" "./autogen/sl_board_default_init.o" "./autogen/sl_cli_command_table.o" "./autogen/sl_cli_instances.o" "./autogen/sl_device_init_clocks.o" "./autogen/sl_event_handler.o" "./autogen/sl_i2cspm_init.o" "./autogen/sl_iostream_handles.o" "./autogen/sl_iostream_init_usart_instances.o" "./autogen/sl_power_manager_handler.o" "./autogen/sl_simple_button_instances.o" "./autogen/sli_cli_hooks.o" "./gecko_sdk_4.5.0/app/common/util/app_log/app_log.o" "./gecko_sdk_4.5.0/app/flex/component/connect/sl_connect_sdk_ota_bootloader_interface/sl_connect_sdk_btl-interface.o" "./gecko_sdk_4.5.0/hardware/board/src/sl_board_control_gpio.o" "./gecko_sdk_4.5.0/hardware/board/src/sl_board_init.o" "./gecko_sdk_4.5.0/hardware/driver/configuration_over_swo/src/sl_cos.o" "./gecko_sdk_4.5.0/hardware/driver/mx25_flash_shutdown/src/sl_mx25_flash_shutdown_usart/sl_mx25_flash_shutdown.o" "./gecko_sdk_4.5.0/hardware/driver/si70xx/src/sl_si70xx.o" "./gecko_sdk_4.5.0/platform/Device/SiliconLabs/EFR32FG12P/Source/startup_efr32fg12p.o" "./gecko_sdk_4.5.0/platform/Device/SiliconLabs/EFR32FG12P/Source/system_efr32fg12p.o" "./gecko_sdk_4.5.0/platform/bootloader/api/btl_interface.o" "./gecko_sdk_4.5.0/platform/bootloader/api/btl_interface_storage.o" "./gecko_sdk_4.5.0/platform/bootloader/app_properties/app_properties.o" "./gecko_sdk_4.5.0/platform/common/src/sl_assert.o" "./gecko_sdk_4.5.0/platform/common/src/sl_slist.o" "./gecko_sdk_4.5.0/platform/common/src/sl_string.o" "./gecko_sdk_4.5.0/platform/common/src/sl_syscalls.o" "./gecko_sdk_4.5.0/platform/common/toolchain/src/sl_memory.o" "./gecko_sdk_4.5.0/platform/driver/button/src/sl_button.o" "./gecko_sdk_4.5.0/platform/driver/button/src/sl_simple_button.o" "./gecko_sdk_4.5.0/platform/driver/debug/src/sl_debug_swo.o" "./gecko_sdk_4.5.0/platform/driver/i2cspm/src/sl_i2cspm.o" "./gecko_sdk_4.5.0/platform/emdrv/dmadrv/src/dmadrv.o" "./gecko_sdk_4.5.0/platform/emdrv/gpiointerrupt/src/gpiointerrupt.o" "./gecko_sdk_4.5.0/platform/emdrv/nvm3/src/nvm3_default_common_linker.o" "./gecko_sdk_4.5.0/platform/emdrv/nvm3/src/nvm3_hal_flash.o" "./gecko_sdk_4.5.0/platform/emdrv/nvm3/src/nvm3_lock.o" "./gecko_sdk_4.5.0/platform/emlib/src/em_cmu.o" "./gecko_sdk_4.5.0/platform/emlib/src/em_core.o" "./gecko_sdk_4.5.0/platform/emlib/src/em_crypto.o" "./gecko_sdk_4.5.0/platform/emlib/src/em_emu.o" "./gecko_sdk_4.5.0/platform/emlib/src/em_gpio.o" "./gecko_sdk_4.5.0/platform/emlib/src/em_i2c.o" "./gecko_sdk_4.5.0/platform/emlib/src/em_ldma.o" "./gecko_sdk_4.5.0/platform/emlib/src/em_msc.o" "./gecko_sdk_4.5.0/platform/emlib/src/em_prs.o" "./gecko_sdk_4.5.0/platform/emlib/src/em_rmu.o" "./gecko_sdk_4.5.0/platform/emlib/src/em_rtcc.o" "./gecko_sdk_4.5.0/platform/emlib/src/em_system.o" "./gecko_sdk_4.5.0/platform/emlib/src/em_timer.o" "./gecko_sdk_4.5.0/platform/emlib/src/em_usart.o" "./gecko_sdk_4.5.0/platform/emlib/src/em_wdog.o" "./gecko_sdk_4.5.0/platform/radio/rail_lib/plugin/pa-conversions/pa_conversions_efr32.o" "./gecko_sdk_4.5.0/platform/radio/rail_lib/plugin/pa-conversions/pa_curves_efr32.o" "./gecko_sdk_4.5.0/platform/radio/rail_lib/plugin/rail_util_power_manager_init/sl_rail_util_power_manager_init.o" "./gecko_sdk_4.5.0/platform/radio/rail_lib/plugin/rail_util_pti/sl_rail_util_pti.o" "./gecko_sdk_4.5.0/platform/security/sl_component/sl_mbedtls_support/src/crypto_aes.o" "./gecko_sdk_4.5.0/platform/security/sl_component/sl_mbedtls_support/src/mbedtls_ccm.o" "./gecko_sdk_4.5.0/platform/security/sl_component/sl_mbedtls_support/src/sl_mbedtls.o" "./gecko_sdk_4.5.0/platform/security/sl_component/sl_mbedtls_support/src/sl_psa_crypto.o" "./gecko_sdk_4.5.0/platform/security/sl_component/sl_mbedtls_support/src/sli_psa_crypto.o" "./gecko_sdk_4.5.0/platform/security/sl_component/sl_psa_driver/src/crypto_management.o" "./gecko_sdk_4.5.0/platform/security/sl_component/sl_psa_driver/src/sl_psa_its_nvm3.o" "./gecko_sdk_4.5.0/platform/security/sl_component/sl_psa_driver/src/sli_crypto_driver_trng.o" "./gecko_sdk_4.5.0/platform/security/sl_component/sl_psa_driver/src/sli_crypto_transparent_driver_aead.o" "./gecko_sdk_4.5.0/platform/security/sl_component/sl_psa_driver/src/sli_crypto_transparent_driver_cipher.o" "./gecko_sdk_4.5.0/platform/security/sl_component/sl_psa_driver/src/sli_crypto_transparent_driver_hash.o" "./gecko_sdk_4.5.0/platform/security/sl_component/sl_psa_driver/src/sli_crypto_transparent_driver_mac.o" "./gecko_sdk_4.5.0/platform/security/sl_component/sl_psa_driver/src/sli_psa_driver_common.o" "./gecko_sdk_4.5.0/platform/security/sl_component/sl_psa_driver/src/sli_psa_driver_init.o" "./gecko_sdk_4.5.0/platform/security/sl_component/sl_psa_driver/src/sli_psa_trng.o" "./gecko_sdk_4.5.0/platform/security/sl_component/sl_psa_driver/src/sli_se_version_dependencies.o" "./gecko_sdk_4.5.0/platform/service/cli/src/sl_cli.o" "./gecko_sdk_4.5.0/platform/service/cli/src/sl_cli_arguments.o" "./gecko_sdk_4.5.0/platform/service/cli/src/sl_cli_command.o" "./gecko_sdk_4.5.0/platform/service/cli/src/sl_cli_input.o" "./gecko_sdk_4.5.0/platform/service/cli/src/sl_cli_io.o" "./gecko_sdk_4.5.0/platform/service/cli/src/sl_cli_tokenize.o" "./gecko_sdk_4.5.0/platform/service/device_init/src/sl_device_init_dcdc_s1.o" "./gecko_sdk_4.5.0/platform/service/device_init/src/sl_device_init_emu_s1.o" "./gecko_sdk_4.5.0/platform/service/device_init/src/sl_device_init_hfxo_s1.o" "./gecko_sdk_4.5.0/platform/service/device_init/src/sl_device_init_lfxo_s1.o" "./gecko_sdk_4.5.0/platform/service/device_init/src/sl_device_init_nvic.o" "./gecko_sdk_4.5.0/platform/service/iostream/src/sl_iostream.o" "./gecko_sdk_4.5.0/platform/service/iostream/src/sl_iostream_retarget_stdio.o" "./gecko_sdk_4.5.0/platform/service/iostream/src/sl_iostream_stdlib_config.o" "./gecko_sdk_4.5.0/platform/service/iostream/src/sl_iostream_uart.o" "./gecko_sdk_4.5.0/platform/service/iostream/src/sl_iostream_usart.o" "./gecko_sdk_4.5.0/platform/service/mpu/src/sl_mpu.o" "./gecko_sdk_4.5.0/platform/service/power_manager/src/sl_power_manager.o" "./gecko_sdk_4.5.0/platform/service/power_manager/src/sl_power_manager_debug.o" "./gecko_sdk_4.5.0/platform/service/power_manager/src/sl_power_manager_hal_s0_s1.o" "./gecko_sdk_4.5.0/platform/service/sleeptimer/src/sl_sleeptimer.o" "./gecko_sdk_4.5.0/platform/service/sleeptimer/src/sl_sleeptimer_hal_rtcc.o" "./gecko_sdk_4.5.0/platform/service/sleeptimer/src/sl_sleeptimer_hal_timer.o" "./gecko_sdk_4.5.0/platform/service/system/src/sl_system_init.o" "./gecko_sdk_4.5.0/platform/service/system/src/sl_system_process_action.o" "./gecko_sdk_4.5.0/platform/service/token_manager/src/sl_token_def.o" "./gecko_sdk_4.5.0/platform/service/token_manager/src/sl_token_manager.o" "./gecko_sdk_4.5.0/platform/service/token_manager/src/sl_token_manufacturing.o" "./gecko_sdk_4.5.0/platform/service/token_manager/src/sl_token_manufacturing_generic.o" "./gecko_sdk_4.5.0/platform/service/udelay/src/sl_udelay.o" "./gecko_sdk_4.5.0/platform/service/udelay/src/sl_udelay_armv6m_gcc.o" "./gecko_sdk_4.5.0/protocol/flex/app-framework-common/app_framework_common.o" "./gecko_sdk_4.5.0/protocol/flex/app-framework-common/app_framework_common_cb.o" "./gecko_sdk_4.5.0/protocol/flex/app-framework-common/app_framework_sleep.o" "./gecko_sdk_4.5.0/protocol/flex/app-framework-common/app_framework_stack_cb.o" "./gecko_sdk_4.5.0/protocol/flex/ota-unicast-bootloader/ota-unicast-bootloader-client/ota-unicast-bootloader-client-cb.o" "./gecko_sdk_4.5.0/protocol/flex/ota-unicast-bootloader/ota-unicast-bootloader-client/ota-unicast-bootloader-client.o" "./gecko_sdk_4.5.0/protocol/flex/poll/poll.o" "./gecko_sdk_4.5.0/protocol/flex/stack/config/ember-configuration.o" "./gecko_sdk_4.5.0/protocol/flex/stack/config/stack-callback-stubs.o" "./gecko_sdk_4.5.0/protocol/flex/stack/config/stack-stubs.o" "./gecko_sdk_4.5.0/protocol/flex/stack/core/sl-connect-watchdog.o" "./gecko_sdk_4.5.0/protocol/flex/stack/core/sli-connect-api.o" "./gecko_sdk_4.5.0/protocol/flex/stack/core/sli-connect-token.o" "./gecko_sdk_4.5.0/protocol/flex/stack/framework/sl-connect-info.o" "./gecko_sdk_4.5.0/protocol/flex/stack/framework/sl-connect-power-manager.o" "./gecko_sdk_4.5.0/protocol/flex/stack/framework/sli-connect-random.o" "./gecko_sdk_4.5.0/util/silicon_labs/silabs_core/memory_manager/sl_malloc.o" "./gecko_sdk_4.5.0/util/third_party/mbedtls/library/aes.o" "./gecko_sdk_4.5.0/util/third_party/mbedtls/library/ccm.o" "./gecko_sdk_4.5.0/util/third_party/mbedtls/library/cipher.o" "./gecko_sdk_4.5.0/util/third_party/mbedtls/library/cipher_wrap.o" "./gecko_sdk_4.5.0/util/third_party/mbedtls/library/constant_time.o" "./gecko_sdk_4.5.0/util/third_party/mbedtls/library/platform.o" "./gecko_sdk_4.5.0/util/third_party/mbedtls/library/platform_util.o" "./gecko_sdk_4.5.0/util/third_party/mbedtls/library/psa_crypto.o" "./gecko_sdk_4.5.0/util/third_party/mbedtls/library/psa_crypto_aead.o" "./gecko_sdk_4.5.0/util/third_party/mbedtls/library/psa_crypto_cipher.o" "./gecko_sdk_4.5.0/util/third_party/mbedtls/library/psa_crypto_client.o" "./gecko_sdk_4.5.0/util/third_party/mbedtls/library/psa_crypto_driver_wrappers_no_static.o" "./gecko_sdk_4.5.0/util/third_party/mbedtls/library/psa_crypto_ecp.o" "./gecko_sdk_4.5.0/util/third_party/mbedtls/library/psa_crypto_ffdh.o" "./gecko_sdk_4.5.0/util/third_party/mbedtls/library/psa_crypto_hash.o" "./gecko_sdk_4.5.0/util/third_party/mbedtls/library/psa_crypto_mac.o" "./gecko_sdk_4.5.0/util/third_party/mbedtls/library/psa_crypto_pake.o" "./gecko_sdk_4.5.0/util/third_party/mbedtls/library/psa_crypto_rsa.o" "./gecko_sdk_4.5.0/util/third_party/mbedtls/library/psa_crypto_se.o" "./gecko_sdk_4.5.0/util/third_party/mbedtls/library/psa_crypto_slot_management.o" "./gecko_sdk_4.5.0/util/third_party/mbedtls/library/psa_crypto_storage.o" "./gecko_sdk_4.5.0/util/third_party/mbedtls/library/psa_util.o" "./gecko_sdk_4.5.0/util/third_party/mbedtls/library/threading.o" "./gecko_sdk_4.5.0/util/third_party/printf/printf.o" "./gecko_sdk_4.5.0/util/third_party/printf/src/iostream_printf.o" "/home/lmh/SimplicityStudio/SDKs/gecko_sdk//protocol/flex/libs/libconnect-aes-security-library-efr32xg1x-gcc.a" "/home/lmh/SimplicityStudio/SDKs/gecko_sdk//protocol/flex/libs/libconnect-frequency-hopping-library-efr32xg1x-gcc.a" "/home/lmh/SimplicityStudio/SDKs/gecko_sdk//protocol/flex/libs/libconnect-packet-queue-library-efr32xg1x-gcc.a" "/home/lmh/SimplicityStudio/SDKs/gecko_sdk//protocol/flex/libs/libconnect-parent-support-library-efr32xg1x-gcc.a" "/home/lmh/SimplicityStudio/SDKs/gecko_sdk//protocol/flex/libs/libconnect-stack-common-library-efr32xg1x-gcc.a" "/home/lmh/SimplicityStudio/SDKs/gecko_sdk//protocol/flex/libs/libconnect-stack-counters-library-efr32xg1x-gcc.a" "/home/lmh/SimplicityStudio/SDKs/gecko_sdk//platform/emdrv/nvm3/lib/libnvm3_CM4_gcc.a" "/home/lmh/SimplicityStudio/SDKs/gecko_sdk//platform/radio/rail_lib/autogen/librail_release/librail_efr32xg12_gcc_release.a" -lgcc -lc -lm -lnosys -Wl,--end-group -Wl,--start-group -lgcc -lc -lnosys -Wl,--end-group
	@echo 'Finished building target: $@'
	@echo ' '

	@echo 'Building out file: cubesat_RX.out'
	arm-none-eabi-objcopy "cubesat_RX.axf" "cubesat_RX.out"
	@echo ' '

	@echo 'Building hex file: cubesat_RX.hex'
	arm-none-eabi-objcopy -O ihex "cubesat_RX.axf" "cubesat_RX.hex"
	@echo ' '

	@echo 'Building bin file: cubesat_RX.bin'
	arm-none-eabi-objcopy -O binary "cubesat_RX.axf" "cubesat_RX.bin"
	@echo ' '

	@echo 'Building s37 file: cubesat_RX.s37'
	arm-none-eabi-objcopy -O srec "cubesat_RX.axf" "cubesat_RX.s37"
	@echo ' '

	@echo 'Running size tool'
	arm-none-eabi-size "cubesat_RX.axf" -A
	@echo ' '

# Other Targets
clean:
	-@$(foreach FILE, $(EXECUTABLES), $(shell $(RM) $(FILE)) )
	-@$(foreach FILE, $(OBJS), $(shell $(RM) $(FILE)) )
	-@$(foreach FILE, $(C_DEPS), $(shell $(RM) $(FILE)) )
	-$(RM) cubesat_RX.axf
	-@echo ' '

.PHONY: all clean dependents main-build

-include ../makefile.targets
